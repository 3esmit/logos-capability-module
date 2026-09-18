#include "capability_module_impl.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <logos_caller.h>
#include <logos_host_services.h>
#include <logos_protocol.h>

#include <cstdio>

namespace {

// The minted value IS the auth token, so this is the one place entropy matters.
// Deliberately the SAME generator the host uses to mint each module's token
// (logos-liblogos module_manager.cpp), rather than a hand-rolled
// std::random_device formatter: boost seeds from the platform CSPRNG
// (/dev/urandom, BCryptGenRandom), whereas std::random_device is permitted to
// be DETERMINISTIC and historically was on MinGW — which is a live target here.
std::string mintToken()
{
    static boost::uuids::random_generator gen;
    return boost::uuids::to_string(gen());
}

// RAII for the per-target client. lp_client_destroy is safe from any thread and
// defers teardown to the owner thread when needed, so an early return cannot
// leak the handle.
class ClientHandle {
public:
    ClientHandle(const std::string& target, const std::string& origin)
        : m_c(lp_client_create(target.c_str(), origin.c_str(), nullptr, nullptr)) {}
    ClientHandle(const std::string& target,
                 const std::string& instance,
                 const std::string& origin)
        : m_c(lp_client_create_instance(target.c_str(), instance.c_str(), origin.c_str(),
                                        nullptr, nullptr)) {}
    ~ClientHandle() { if (m_c) lp_client_destroy(m_c); }
    ClientHandle(const ClientHandle&) = delete;
    ClientHandle& operator=(const ClientHandle&) = delete;

    lp_client* get() const { return m_c; }
    explicit operator bool() const { return m_c != nullptr; }

private:
    lp_client* m_c = nullptr;
};

void warn(const char* fmt, const std::string& a, const std::string& b = {})
{
    std::fprintf(stderr, fmt, a.c_str(), b.c_str());
}

// A module that calls out from its own initializer has not published its
// source yet, so this one grant can be unsatisfiable. Waiting the protocol
// default (20s) there would blow every startup deadline downstream — the
// standalone app gives a ui-host 10s to report ready — and turn one
// unreachable module into a dead UI. Fail fast instead: the caller gets no
// token and a clear reason, and the rest of startup keeps moving.
constexpr int kTokenPushTimeoutMs = 3000;

// Keep explicit-instance bootstrap credentials in an injective namespace. The
// host's Qt-side helper uses the same spelling for the ASCII module/instance
// identifiers used by the runtime; lengths prevent delimiter ambiguity.
std::string scopedTokenKey(const std::string& moduleName,
                           const std::string& instanceId)
{
    return "logos.instance-token.v1/" + std::to_string(moduleName.size()) + ":" +
           moduleName + "/" + std::to_string(instanceId.size()) + ":" + instanceId;
}

} // namespace

std::string CapabilityModuleImpl::requestModule(const std::string& fromModuleName,
                                                const std::string& moduleName)
{
    // Target emptiness is checked first so a missing name cannot be papered
    // over by a well-formed caller identity (or vice versa).
    if (moduleName.empty()) {
        warn("[capability_module] rejecting empty target module name (from='%s')\n",
             fromModuleName);
        return {};
    }

    // Identity comes from the RPC caller document the host pushed into this
    // image (logos_module_set_call_caller / logos::currentCaller), not from
    // `fromModuleName`. That argument is leftover ABI: any loaded allowlisted
    // name could be written there by the caller. Host maps to "core" (rule 5:
    // the host arm carries no name). Unnamed / unknown / derived / operator
    // refuse — those are not module identities this method can mint for.
    const logos::LogosCaller caller = logos::currentCaller();
    std::string callerName;
    if (caller.isHost()) {
        callerName = "core";
    } else if (caller.isModule() && !caller.name.empty()) {
        callerName = caller.name;
    } else {
        warn("[capability_module] rejecting request for '%s': no named caller on "
             "this dispatch (fromModuleName='%s')\n",
             moduleName, fromModuleName);
        return {};
    }
    if (!fromModuleName.empty() && fromModuleName != callerName) {
        warn("[capability_module] ignoring leftover fromModuleName='%s' "
             "(token-bound caller is '%s')\n",
             fromModuleName, callerName);
    }

    // token_registry remains load-bearing: tokenFor() below reads the registry,
    // and an ungranted image must fail closed rather than looking like "the
    // target is not loaded". The explicit status check (not empty()) is what
    // distinguishes those two refusals.
    logos::host::Status keysStatus;
    (void)logos::host::tokenKeys(&keysStatus);
    if (keysStatus.ungranted()) {
        warn("[capability_module] REFUSING '%s': this module was not granted the "
             "token_registry host service, so it cannot look up the target\n",
             callerName);
        return {};
    }

    // Known-target gate: no token for the target means it is not loaded. Don't
    // hand back a token the target would reject anyway.
    const std::string moduleToken = logos::host::tokenFor(moduleName);
    if (moduleToken.empty()) {
        warn("[capability_module] rejecting request for unknown target '%s' "
             "- no token registered for it\n", moduleName);
        return {};
    }

    // Access-policy gate.
    //
    // TODO(access-policy): still fail-OPEN — a target with no registered
    // restriction is unrestricted. Intentional for back-compat during rollout;
    // the end state is deny-by-default once every deployment ships a policy.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_restrictions.find(moduleName);
        if (it != m_restrictions.end() && it->second.count(callerName) == 0) {
            warn("[capability_module] access policy denies '%s' -> '%s'\n",
                 callerName, moduleName);
            return {};
        }
    }

    const std::string authToken = mintToken();

    ClientHandle client(moduleName, "capability_module");
    if (!client) {
        warn("[capability_module] could not create a client for target '%s'\n", moduleName);
        return {};
    }

    // Deliver the token for the REQUESTER to the TARGET.
    //
    // The argument order is the trap here, so spell it out: authenticate with
    // the TARGET's own token, `originModule` is the TARGET (the module being
    // told), and `moduleName` is the REQUESTER (the module the token is FOR).
    // Swapping the last two still compiles and still returns an ok-shaped
    // status; it just tells the wrong module about the wrong token.
    const logos::host::Status pushed = logos::host::informModuleTokenTo(
        client.get(),
        /*authToken=*/moduleToken,
        /*originModule=*/moduleName,
        /*moduleName=*/callerName,
        /*token=*/authToken,
        kTokenPushTimeoutMs);

    if (!pushed) {
        if (pushed.ungranted()) {
            warn("[capability_module] REFUSING '%s': this module was not granted the "
                 "token_delivery host service, so it cannot push tokens\n", moduleName);
        } else {
            warn("[capability_module] failed to inform '%s' about the token for '%s'\n",
                 moduleName, callerName);
        }
        return {};
    }

    return authToken;
}

std::string CapabilityModuleImpl::requestModuleScoped(
    const std::string& fromModuleName,
    const std::string& moduleName,
    const std::string& instanceId)
{
    if (moduleName.empty() || instanceId.empty()) {
        warn("[capability_module] rejecting empty scoped target (module='%s')\n",
             moduleName, instanceId);
        return {};
    }

    const logos::LogosCaller caller = logos::currentCaller();
    std::string callerName;
    if (caller.isHost()) {
        callerName = "core";
    } else if (caller.isModule() && !caller.name.empty()) {
        callerName = caller.name;
    } else {
        warn("[capability_module] rejecting scoped request for '%s': no named caller\n",
             moduleName, instanceId);
        return {};
    }
    if (!fromModuleName.empty() && fromModuleName != callerName) {
        warn("[capability_module] ignoring scoped fromModuleName='%s' (token-bound caller='%s')\n",
             fromModuleName, callerName);
    }

    logos::host::Status keysStatus;
    (void)logos::host::tokenKeys(&keysStatus);
    if (keysStatus.ungranted()) {
        warn("[capability_module] REFUSING scoped request from '%s': token_registry not granted\n",
             callerName, moduleName);
        return {};
    }

    const std::string targetKey = scopedTokenKey(moduleName, instanceId);
    const std::string moduleToken = logos::host::tokenFor(targetKey);
    if (moduleToken.empty()) {
        warn("[capability_module] rejecting unknown scoped target '%s' instance '%s'\n",
             moduleName, instanceId);
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_restrictions.find(moduleName);
        if (it != m_restrictions.end() && it->second.count(callerName) == 0) {
            warn("[capability_module] scoped access policy denies '%s' -> '%s'\n",
                 callerName, moduleName);
            return {};
        }
    }

    const std::string authToken = mintToken();
    ClientHandle client(moduleName, instanceId, "capability_module");
    if (!client) {
        warn("[capability_module] could not create client for scoped target '%s'\n",
             moduleName, instanceId);
        return {};
    }

    const logos::host::Status pushed = logos::host::informModuleTokenTo(
        client.get(), moduleToken, moduleName, callerName, authToken, kTokenPushTimeoutMs);
    if (!pushed) {
        warn("[capability_module] failed to inform scoped target '%s' instance '%s'\n",
             moduleName, instanceId);
        return {};
    }
    return authToken;
}

bool CapabilityModuleImpl::informModuleTokenScoped(const std::string& authToken,
                                                   const std::string& moduleName,
                                                   const std::string& instanceId,
                                                   const std::string& token)
{
    if (authToken.empty() || moduleName.empty() || instanceId.empty() || token.empty()) {
        warn("[capability_module] rejecting empty scoped bootstrap for '%s'\n",
             moduleName, instanceId);
        return false;
    }

    const std::string coreToken = logos::host::tokenFor("core");
    const std::string capToken = logos::host::tokenFor("capability_module");
    const bool callerIsTrusted =
        (!coreToken.empty() && logos::host::constantTimeEquals(authToken, coreToken)) ||
        (!capToken.empty() && logos::host::constantTimeEquals(authToken, capToken));
    if (!callerIsTrusted) {
        warn("[capability_module] rejecting scoped bootstrap for '%s' instance '%s'\n",
             moduleName, instanceId);
        return false;
    }

    const std::string key = scopedTokenKey(moduleName, instanceId);
    return lp_token_save(key.c_str(), token.c_str()) == LP_OK;
}

bool CapabilityModuleImpl::registerRestriction(const std::string& authToken,
                                               const std::string& targetModule,
                                               const std::vector<std::string>& allowedCallers)
{
    // Trusted-channel gate: only core (or this module) may rewrite the policy.
    // Both hold this module's auth token; a peer knows only its own. The
    // generic authorization that fronts this method accepts ANY issued token,
    // which would otherwise let any module rewrite the policy.
    //
    // constantTimeEquals, not ==: comparing a secret with == leaks the length
    // of the matching prefix through timing.
    const std::string coreToken = logos::host::tokenFor("core");
    const std::string capToken  = logos::host::tokenFor("capability_module");
    const bool callerIsTrusted =
        (!coreToken.empty() && logos::host::constantTimeEquals(authToken, coreToken)) ||
        (!capToken.empty()  && logos::host::constantTimeEquals(authToken, capToken));
    if (authToken.empty() || !callerIsTrusted) {
        warn("[capability_module] rejecting restriction for '%s' - caller is not the "
             "trusted core channel\n", targetModule);
        return false;
    }

    if (targetModule.empty()) {
        warn("[capability_module] rejecting empty target module%s\n", std::string());
        return false;
    }

    // Overwrite any previous restriction — core is the single source of truth
    // and re-registers the full set each boot.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_restrictions[targetModule] =
            std::set<std::string>(allowedCallers.begin(), allowedCallers.end());
    }
    return true;
}

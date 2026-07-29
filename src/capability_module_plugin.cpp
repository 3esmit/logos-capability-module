#include "capability_module_plugin.h"

#include <QByteArray>
#include <QDebug>
#include <QUuid>

#include <algorithm>

#include "logos_api_client.h"
#include "token_manager.h"

namespace {

// Constant-time token comparison: compare over the longer of the two lengths
// and fold any length difference into the result, so neither a correct prefix
// nor the secret's length leaks through timing.
bool constantTimeEquals(const QString& a, const QString& b)
{
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    const int n = std::max(ba.size(), bb.size());
    int diff = ba.size() ^ bb.size();
    for (int i = 0; i < n; ++i) {
        const unsigned char ca = i < ba.size() ? static_cast<unsigned char>(ba[i]) : 0;
        const unsigned char cb = i < bb.size() ? static_cast<unsigned char>(bb[i]) : 0;
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

} // namespace

CapabilityModulePlugin::CapabilityModulePlugin(QObject* parent)
    : QObject(parent)
{
    qDebug() << "CapabilityModulePlugin: created";
}

CapabilityModulePlugin::~CapabilityModulePlugin()
{
    qDebug() << "CapabilityModulePlugin: destroyed";
}

void CapabilityModulePlugin::initLogos(LogosAPI* logosAPIInstance)
{
    logosAPI = logosAPIInstance;
    qDebug() << "CapabilityModulePlugin: LogosAPI initialized";
}

QString CapabilityModulePlugin::requestModule(const QString& fromModuleName, const QString& moduleName)
{
    qDebug() << "CapabilityModulePlugin::requestModule called with fromModuleName:" << fromModuleName
             << "moduleName:" << moduleName;

    if (!logosAPI) {
        qWarning() << "CapabilityModulePlugin::requestModule: LogosAPI not initialized";
        return {};
    }

    if (fromModuleName.isEmpty() || moduleName.isEmpty()) {
        qWarning() << "CapabilityModulePlugin::requestModule: rejecting empty module name"
                   << "(fromModuleName / moduleName must both be set)";
        return {};
    }

    TokenManager* tokenManager = logosAPI->getTokenManager();

    // Known-caller gate: the requesting identity must be a module capability_module
    // already knows about. Fail closed on an unknown name rather than mint a token
    // for a self-asserted identity that was never loaded.
    if (!tokenManager->getTokenKeys().contains(fromModuleName)) {
        qWarning() << "CapabilityModulePlugin::requestModule: rejecting request from unknown"
                   << "module identity:" << fromModuleName
                   << "- no token registered for it (unverified requesting identity)";
        return {};
    }

    // Known-target gate: an empty target token means the target is not loaded /
    // unknown. Don't hand back a token the target would reject anyway — fail closed.
    const QString moduleToken = tokenManager->getToken(moduleName);
    if (moduleToken.isEmpty()) {
        qWarning() << "CapabilityModulePlugin::requestModule: rejecting request for unknown"
                   << "target module:" << moduleName << "- no token registered for it";
        return {};
    }

    // Access-policy gate: if core registered a restriction for this target,
    // only the listed callers may obtain a token. A target with no registered
    // restriction is unrestricted (back-compat). Fail closed for a restricted
    // target — the denied caller never gets a token, so it can never call it.
    //
    // TODO(access-policy): this is currently fail-OPEN — when m_restrictions is
    // empty (no policy pushed, e.g. running against a core that doesn't push
    // restrictions) nothing is enforced and every caller is allowed. This is
    // intentional for back-compat during rollout, but the end state should be
    // deny-by-default: once every deployment ships a policy, an empty/unknown
    // policy should block inter-module calls rather than permit them. Revisit
    // and flip the default once the policy is guaranteed to be present.
    if (auto it = m_restrictions.constFind(moduleName); it != m_restrictions.constEnd()) {
        if (!it->contains(fromModuleName)) {
            qWarning() << "CapabilityModulePlugin::requestModule: access policy denies"
                       << fromModuleName << "->" << moduleName
                       << "- caller not in the allowed set";
            return {};
        }
    }

    const QString authTokenString = QUuid::createUuid().toString(QUuid::WithoutBraces);

    qDebug() << "CapabilityModulePlugin: Calling informModuleToken on target module:" << moduleName;

    const bool success = logosAPI->getClient(moduleName)->informModuleToken_module(
        moduleToken, moduleName, fromModuleName, authTokenString);
    if (!success) {
        qWarning() << "CapabilityModulePlugin: Failed to inform" << moduleName
                   << "about token for" << fromModuleName;
        return {};
    }

    qDebug() << "CapabilityModulePlugin: Successfully informed" << moduleName
             << "about token for" << fromModuleName;
    return authTokenString;
}

QString CapabilityModulePlugin::requestModuleScoped(const QString& fromModuleName,
                                                     const QString& moduleName,
                                                     const QString& instanceId)
{
    qDebug() << "CapabilityModulePlugin::requestModuleScoped called with fromModuleName:"
             << fromModuleName << "moduleName:" << moduleName
             << "instanceId:" << instanceId;

    if (!logosAPI) {
        qWarning() << "CapabilityModulePlugin::requestModuleScoped: LogosAPI not initialized";
        return {};
    }

    if (fromModuleName.isEmpty() || moduleName.isEmpty() || instanceId.isEmpty()) {
        qWarning() << "CapabilityModulePlugin::requestModuleScoped: rejecting empty module or instance identity";
        return {};
    }

    TokenManager* tokenManager = logosAPI->getTokenManager();

    if (!tokenManager->getTokenKeys().contains(fromModuleName)) {
        qWarning() << "CapabilityModulePlugin::requestModuleScoped: rejecting request from unknown"
                   << "module identity:" << fromModuleName;
        return {};
    }

    const QString targetKey = logos::scopedModuleTokenKey(moduleName, instanceId);
    const QString moduleToken = tokenManager->getToken(targetKey);
    if (moduleToken.isEmpty()) {
        qWarning() << "CapabilityModulePlugin::requestModuleScoped: rejecting request for unknown"
                   << "target module instance:" << moduleName << instanceId;
        return {};
    }

    if (auto it = m_restrictions.constFind(moduleName); it != m_restrictions.constEnd()) {
        if (!it->contains(fromModuleName)) {
            qWarning() << "CapabilityModulePlugin::requestModuleScoped: access policy denies"
                       << fromModuleName << "->" << moduleName << instanceId;
            return {};
        }
    }

    const QString authTokenString = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LogosAPIClient* targetClient = logosAPI->getClient(moduleName, instanceId);
    if (!targetClient) {
        qWarning() << "CapabilityModulePlugin::requestModuleScoped: failed to create target client for"
                   << moduleName << instanceId;
        return {};
    }

    const bool success = targetClient->informModuleToken_module(
        moduleToken, moduleName, fromModuleName, authTokenString);
    if (!success) {
        qWarning() << "CapabilityModulePlugin::requestModuleScoped: failed to inform"
                   << moduleName << "instance" << instanceId
                   << "about token for" << fromModuleName;
        return {};
    }

    qDebug() << "CapabilityModulePlugin::requestModuleScoped: successfully informed"
             << moduleName << "instance" << instanceId
             << "about token for" << fromModuleName;
    return authTokenString;
}

bool CapabilityModulePlugin::informModuleTokenScoped(const QString& authToken,
                                                      const QString& moduleName,
                                                      const QString& instanceId,
                                                      const QString& token)
{
    if (!logosAPI) {
        qWarning() << "CapabilityModulePlugin::informModuleTokenScoped: LogosAPI not initialized";
        return false;
    }

    if (moduleName.isEmpty() || instanceId.isEmpty() || token.isEmpty()) {
        qWarning() << "CapabilityModulePlugin::informModuleTokenScoped: rejecting empty module, instance, or token";
        return false;
    }

    TokenManager* tokenManager = logosAPI->getTokenManager();
    const QString coreToken = tokenManager->getToken(QStringLiteral("core"));
    const QString capToken = tokenManager->getToken(QStringLiteral("capability_module"));
    const bool callerIsTrusted =
        (!coreToken.isEmpty() && constantTimeEquals(authToken, coreToken)) ||
        (!capToken.isEmpty() && constantTimeEquals(authToken, capToken));
    if (authToken.isEmpty() || !callerIsTrusted) {
        qWarning() << "CapabilityModulePlugin::informModuleTokenScoped: rejecting bootstrap token for"
                   << moduleName << "instance" << instanceId
                   << "from an untrusted caller";
        return false;
    }

    tokenManager->saveToken(logos::scopedModuleTokenKey(moduleName, instanceId), token);
    qDebug() << "CapabilityModulePlugin::informModuleTokenScoped: stored bootstrap token for"
             << moduleName << "instance" << instanceId;
    return true;
}

bool CapabilityModulePlugin::registerRestriction(const QString& authToken,
                                                 const QString& targetModule,
                                                 const QStringList& allowedCallers)
{
    if (!logosAPI) {
        qWarning() << "CapabilityModulePlugin::registerRestriction: LogosAPI not initialized";
        return false;
    }

    // Trusted-channel gate: only core (or capability_module itself) may
    // register restrictions. Both hold capability_module's auth token; a
    // peer module only knows its own token and so cannot forge this. The
    // generic isAuthorized() that fronts this method accepts ANY issued
    // token, which would otherwise let a malicious module rewrite the policy.
    TokenManager* tokenManager = logosAPI->getTokenManager();
    const QString coreToken = tokenManager->getToken(QStringLiteral("core"));
    const QString capToken  = tokenManager->getToken(QStringLiteral("capability_module"));
    const bool callerIsTrusted =
        (!coreToken.isEmpty() && constantTimeEquals(authToken, coreToken)) ||
        (!capToken.isEmpty()  && constantTimeEquals(authToken, capToken));
    if (authToken.isEmpty() || !callerIsTrusted) {
        qWarning() << "CapabilityModulePlugin::registerRestriction: rejecting restriction for"
                   << targetModule << "- caller is not the trusted core channel";
        return false;
    }

    if (targetModule.isEmpty()) {
        qWarning() << "CapabilityModulePlugin::registerRestriction: rejecting empty target module";
        return false;
    }

    // Overwrite any previous restriction for this target — core is the
    // single source of truth and re-registers the full set each boot.
    m_restrictions.insert(targetModule, QSet<QString>(allowedCallers.begin(), allowedCallers.end()));

    qDebug() << "CapabilityModulePlugin::registerRestriction: target" << targetModule
             << "restricted to callers" << allowedCallers;
    return true;
}

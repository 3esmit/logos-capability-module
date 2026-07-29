#ifndef CAPABILITY_MODULE_PLUGIN_H
#define CAPABILITY_MODULE_PLUGIN_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "capability_module_interface.h"
#include "logos_api.h"

class CapabilityModulePlugin : public QObject, public CapabilityModuleInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID CapabilityModuleInterface_iid FILE "metadata.json")
    Q_INTERFACES(CapabilityModuleInterface PluginInterface)

public:
    explicit CapabilityModulePlugin(QObject* parent = nullptr);
    ~CapabilityModulePlugin() override;

    QString name() const override { return QStringLiteral("capability_module"); }
    QString version() const override { return QStringLiteral("1.0.0"); }

    Q_INVOKABLE void initLogos(LogosAPI* logosAPIInstance);

    Q_INVOKABLE QString requestModule(const QString& fromModuleName,
                                      const QString& moduleName) override;

    // Scoped counterpart used only when the target is an explicit runtime
    // instance. The legacy name-only requestModule API remains unchanged.
    Q_INVOKABLE QString requestModuleScoped(const QString& fromModuleName,
                                            const QString& moduleName,
                                            const QString& instanceId);

    // Register a target bootstrap token for one explicit runtime instance.
    // authToken is repeated from the transport envelope so this provider can
    // enforce its privileged core/capability-channel check.
    Q_INVOKABLE bool informModuleTokenScoped(const QString& authToken,
                                             const QString& moduleName,
                                             const QString& instanceId,
                                             const QString& token);

    Q_INVOKABLE bool registerRestriction(const QString& authToken,
                                         const QString& targetModule,
                                         const QStringList& allowedCallers) override;

private:
    // target module -> set of caller modules allowed to reach it. A target
    // absent from this map is unrestricted (back-compat default). Populated
    // only by registerRestriction; consulted in requestModule.
    QHash<QString, QSet<QString>> m_restrictions;
};

#endif // CAPABILITY_MODULE_PLUGIN_H

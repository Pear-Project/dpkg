#include "updatemanager.h"
#include <QProcess>

UpdateManager::UpdateManager(QObject *parent) : QObject(parent) {}

void UpdateManager::run(const QString &cmd, std::function<void(QString)> cb) {
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [proc, cb](int, QProcess::ExitStatus) {
        cb(QString::fromUtf8(proc->readAllStandardOutput()));
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, proc, &QProcess::deleteLater);
    proc->start("bash", {"-c", cmd});
}

void UpdateManager::runStreaming(const QString &cmd) {
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        m_output += QString::fromUtf8(proc->readAllStandardOutput());
        emit outputChanged();
    });
    connect(proc, &QProcess::finished, this, [this, proc](int, QProcess::ExitStatus) {
        m_updating = false;
        emit stateChanged();
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError) {
        m_updating = false;
        emit stateChanged();
        proc->deleteLater();
    });
    proc->start("bash", {"-c", cmd});
}

// `apt list --upgradable` lines look like:
//   firefox/noble-updates 121.0-1 amd64 [upgradable from: 120.0-1]
// (plus a "Listing..." header line to skip). No apt-get update is run here
// on purpose -- same "check against whatever's already known" semantics as
// the Arch edition's checkupdates/pacman -Qu, not a privileged sync.
void UpdateManager::checkUpdates() {
    if (m_checking) return;
    m_checking = true;
    m_updates.clear();
    emit stateChanged();

    run("apt list --upgradable 2>/dev/null", [this](QString out) {
        m_checking = false;
        for (const QString &line : out.split('\n')) {
            QString l = line.trimmed();
            if (l.isEmpty() || l.startsWith("Listing...")) continue;

            QStringList parts = l.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) continue;

            QVariantMap pkg;
            pkg["name"]    = parts.value(0).section('/', 0, 0);
            pkg["latest"]  = parts.value(1);

            QString current;
            int idx = l.indexOf(QStringLiteral("upgradable from:"));
            if (idx >= 0) {
                current = l.mid(idx + QStringLiteral("upgradable from:").length());
                current.remove(QLatin1Char(']'));
                current = current.trimmed();
            }
            pkg["current"] = current;

            m_updates.append(pkg);
        }
        emit stateChanged();
    });
}

void UpdateManager::applyUpdates() {
    if (m_updating) return;
    m_updating = true;
    m_output.clear();
    emit stateChanged();
    runStreaming("pkexec sh -c 'apt-get update && apt-get -y full-upgrade' 2>&1");
}

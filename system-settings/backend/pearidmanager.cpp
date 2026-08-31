#include "pearidmanager.h"
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QVariantMap>
#include <QCryptographicHash>

// Persisted "was last known logged in" marker, so a network hiccup on the
// very first check of a session doesn't have to guess between logged-in and
// logged-out — it can fall back to what was last confirmed by the server.
static QString lastOkMarkerPath() {
    QString dir = QDir::homePath() + "/.cache/pearos-settings";
    QDir().mkpath(dir);
    return dir + "/pearid_last_ok";
}

static QByteArray fileHash(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

static QString findScriptDir() {
    QString installed = "/usr/share/extras/system-settings/pearID";
    if (QDir(installed).exists()) return installed;
    QString src = QDir::homePath() + "/Desktop/pkgbuilds/pearos-settings-app/pearID";
    if (QDir(src).exists()) return src;
    return installed;
}

PearIDManager::PearIDManager(QObject *parent) : QObject(parent) {
    m_scriptDir = findScriptDir();
}

void PearIDManager::run(const QString &cmd, std::function<void(QString, int)> cb) {
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [proc, cb](int code, QProcess::ExitStatus) {
        cb(QString::fromUtf8(proc->readAllStandardOutput()), code);
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, cb](QProcess::ProcessError) {
        cb(QString(), -1);
        proc->deleteLater();
    });
    proc->start("bash", {"-c", cmd});
}

void PearIDManager::checkState() {
    m_state = "loading";
    emit stateChanged();
    QString stateScript = m_scriptDir + "/state.sh";
    if (!QFile::exists(stateScript)) {
        m_state = "loggedout";
        emit stateChanged();
        return;
    }
    run("bash \"" + stateScript + "\"", [this](QString out, int) {
        QString result = out.trimmed();
        if (result == "true") {
            m_state = "loggedin";
            QFile marker(lastOkMarkerPath());
            (void)marker.open(QIODevice::WriteOnly);
            emit stateChanged();
            fetchUserInfo();
        } else if (result == "false") {
            // Explicit rejection from the server (or no token at all) — really logged out.
            m_state = "loggedout";
            QFile::remove(lastOkMarkerPath());
            emit stateChanged();
        } else {
            // "300" (no network) / "500" (server error) / anything else: state.sh
            // couldn't actually verify the token either way. Treating this the
            // same as "false" is what made login look flaky — a slow API response
            // or DNS hiccup would flip an otherwise valid session to "please log
            // in". Keep whatever we last knew to be true instead of guessing.
            if (m_state == "loading") {
                m_state = QFile::exists(lastOkMarkerPath()) ? "loggedin" : "loggedout";
            }
            emit stateChanged();
        }
    });
}

void PearIDManager::fetchUserInfo() {
    QString infoScript = m_scriptDir + "/get_user_info.sh";
    if (!QFile::exists(infoScript)) return;

    // Core info: first_name, last_name, email (plain lines, no labels)
    run("bash \"" + infoScript + "\" --first-name --last-name --email", [this](QString out, int) {
        QStringList lines;
        for (const QString &l : out.split('\n')) {
            QString t = l.trimmed();
            if (!t.isEmpty()) lines << t;
        }
        m_userName  = (lines.value(0) + " " + lines.value(1)).trimmed();
        if (m_userName.isEmpty()) m_userName = lines.value(2);
        m_userEmail = lines.value(2);
        emit userInfoChanged();
    });

    // Avatar download
    run("bash \"" + infoScript + "\" --avatar 2>/dev/null", [this](QString, int code) {
        if (code == 0) {
            QString path = QDir::homePath() + "/.pearid_avatars/avatar.webp";
            if (QFile::exists(path)) {
                m_avatarPath = path;
                emit userInfoChanged();
                syncAvatarSystemWideIfChanged(path);
            }
        }
    });
}

// The download above only ever landed the PearID avatar in
// ~/.pearid_avatars/avatar.webp for this class's own m_avatarPath (used by
// the QML account page) - it never reached the actual places the rest of
// the system reads a user's picture from: ~/.face.icon, AccountsService
// (what SDDM's user list reads - homes are 0700, so the sddm daemon user
// can't stat ~/.face.icon directly), or the SDDM theme faces/images
// fallback. Those are exactly the targets post_setup writes at install
// time for the wizard-chosen picture, which is why the PearID avatar never
// visibly "took" anywhere that matters. Hash-gated so this doesn't pkexec-
// prompt on every login/Settings-open once the two are already in sync -
// only when the PearID avatar actually differs from what's currently set.
void PearIDManager::syncAvatarSystemWideIfChanged(const QString &avatarPath) {
    QString facePath = QDir::homePath() + "/.face.icon";
    if (QFile::exists(facePath) && fileHash(facePath) == fileHash(avatarPath)) {
        return;
    }

    QString username = qEnvironmentVariable("USER");
    if (username.isEmpty()) username = qEnvironmentVariable("LOGNAME");
    if (username.isEmpty()) return;

    // ~/.face.icon lives in the user's own home - no privilege needed.
    QFile::remove(facePath);
    QFile::copy(avatarPath, facePath);

    // Everything else lives under root-owned paths - only reachable via pkexec.
    QString escAvatar = QString(avatarPath).replace("'", "'\\''");
    QString escUser   = QString(username).replace("'", "'\\''");
    QString cmd = QString(
        "mkdir -p /var/lib/AccountsService/icons /var/lib/AccountsService/users && "
        "cp -f '%1' '/var/lib/AccountsService/icons/%2' && "
        "chmod 0644 '/var/lib/AccountsService/icons/%2' && "
        "printf '[User]\\nIcon=/var/lib/AccountsService/icons/%2\\nSystemAccount=false\\n' "
        "> '/var/lib/AccountsService/users/%2' && "
        "chmod 0600 '/var/lib/AccountsService/users/%2' && "
        "for t in pearOS pearOS-dark; do for s in faces images; do "
        "mkdir -p \"/usr/share/sddm/themes/$t/$s\" && "
        "cp -f '%1' \"/usr/share/sddm/themes/$t/$s/.face.icon\"; done; done"
    ).arg(escAvatar, escUser);

    auto *proc = new QProcess(this);
    proc->start("pkexec", {"bash", "-c", cmd});
    connect(proc, &QProcess::finished, this, [proc](int, QProcess::ExitStatus) {
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, proc, &QProcess::deleteLater);
}

void PearIDManager::fetchExtendedInfo() {
    QString infoScript = m_scriptDir + "/get_user_info.sh";
    if (!QFile::exists(infoScript)) return;

    // Script outputs in fixed order regardless of flag order: phone, billing, birthdate
    run("bash \"" + infoScript + "\" --phone --birthdate --billing-address", [this](QString out, int) {
        QStringList lines;
        for (const QString &l : out.split('\n')) {
            QString t = l.trimmed();
            if (!t.isEmpty()) lines << t;
        }
        m_phone          = lines.value(0);
        m_billingAddress = lines.value(1);
        m_birthdate      = lines.value(2);
        emit userInfoChanged();
    });
}

void PearIDManager::fetchDevices() {
    QString infoScript = m_scriptDir + "/get_user_info.sh";
    if (!QFile::exists(infoScript)) return;

    run("bash \"" + infoScript + "\" --devices", [this](QString out, int) {
        m_devices.clear();
        for (const QString &line : out.split('\n')) {
            QString t = line.trimmed();
            if (!t.isEmpty()) {
                QVariantMap entry;
                entry["name"] = t;
                m_devices.append(entry);
            }
        }
        emit devicesChanged();
    });
}

void PearIDManager::fetchApps() {
    QString infoScript = m_scriptDir + "/get_user_info.sh";
    if (!QFile::exists(infoScript)) return;

    run("bash \"" + infoScript + "\" --apps", [this](QString out, int) {
        m_apps.clear();
        for (const QString &line : out.split('\n')) {
            QString t = line.trimmed();
            if (!t.isEmpty()) {
                QVariantMap entry;
                entry["name"] = t;
                m_apps.append(entry);
            }
        }
        emit appsChanged();
    });
}

void PearIDManager::updateName(const QString &firstName, const QString &lastName) {
    QString updateScript = m_scriptDir + "/update_user_info.sh";
    if (!QFile::exists(updateScript)) {
        emit updateResult("name", false, "Script not found");
        return;
    }
    QString cmd = "bash \"" + updateScript + "\" --first-name \"" +
                  firstName.trimmed() + "\" --last-name \"" + lastName.trimmed() + "\"";
    run(cmd, [this, firstName, lastName](QString out, int code) {
        bool ok = (code == 0);
        if (ok) {
            m_userName = (firstName + " " + lastName).trimmed();
            emit userInfoChanged();
        }
        emit updateResult("name", ok, ok ? QString() : out.trimmed());
    });
}

void PearIDManager::updatePhone(const QString &phone) {
    QString updateScript = m_scriptDir + "/update_user_info.sh";
    if (!QFile::exists(updateScript)) {
        emit updateResult("phone", false, "Script not found");
        return;
    }
    run("bash \"" + updateScript + "\" --phone \"" + phone + "\"", [this, phone](QString out, int code) {
        bool ok = (code == 0);
        if (ok) {
            m_phone = phone;
            emit userInfoChanged();
        }
        emit updateResult("phone", ok, ok ? QString() : out.trimmed());
    });
}

void PearIDManager::updateBillingAddress(const QString &address) {
    QString updateScript = m_scriptDir + "/update_user_info.sh";
    if (!QFile::exists(updateScript)) {
        emit updateResult("billing", false, "Script not found");
        return;
    }
    run("bash \"" + updateScript + "\" --billing-address \"" + address + "\"", [this, address](QString out, int code) {
        bool ok = (code == 0);
        if (ok) {
            m_billingAddress = address;
            emit userInfoChanged();
        }
        emit updateResult("billing", ok, ok ? QString() : out.trimmed());
    });
}

void PearIDManager::changePassword(const QString &oldPw, const QString &newPw) {
    QString updateScript = m_scriptDir + "/update_user_info.sh";
    if (!QFile::exists(updateScript)) {
        emit updateResult("password", false, "Script not found");
        return;
    }
    QString safe_old = QString(oldPw).replace("\"", "\\\"");
    QString safe_new = QString(newPw).replace("\"", "\\\"");
    QString cmd = "bash \"" + updateScript + "\" --old-password \"" + safe_old + "\" --new-password \"" + safe_new + "\"";
    run(cmd, [this](QString out, int code) {
        bool ok = (code == 0);
        emit updateResult("password", ok, ok ? QString() : out.trimmed());
    });
}

void PearIDManager::login(const QString &email, const QString &password) {
    QString loginScript = m_scriptDir + "/login_and_sync.sh";
    if (!QFile::exists(loginScript)) {
        emit loginResult(false, "Login script not found");
        return;
    }
    QString safeEmail    = QString(email).replace("\"", "\\\"");
    QString safePassword = QString(password).replace("\"", "\\\"");
    run("bash \"" + loginScript + "\" \"" + safeEmail + "\" \"" + safePassword + "\"",
        [this](QString out, int code) {
        bool ok = (code == 0) || out.contains("Authentication successful") || out.contains("Authenticated");
        if (ok) {
            m_state = "loggedin";
            QFile marker(lastOkMarkerPath());
            (void)marker.open(QIODevice::WriteOnly);
            emit stateChanged();
            fetchUserInfo();
        }
        emit loginResult(ok, ok ? QString() : "Login failed. Check your credentials.");
    });
}

void PearIDManager::logout() {
    QString exitScript = m_scriptDir + "/exit.sh";
    auto finish = [this](QString, int) {
        m_state = "loggedout";
        QFile::remove(lastOkMarkerPath());
        m_userName.clear();
        m_userEmail.clear();
        m_avatarPath.clear();
        m_phone.clear();
        m_birthdate.clear();
        m_billingAddress.clear();
        m_devices.clear();
        m_apps.clear();
        emit stateChanged();
        emit userInfoChanged();
        emit devicesChanged();
        emit appsChanged();
    };
    if (QFile::exists(exitScript)) {
        run("bash \"" + exitScript + "\"", finish);
    } else {
        finish({}, 0);
    }
}

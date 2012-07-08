/* ============================================================
* GreaseMonkey plugin for QupZilla
* Copyright (C) 2012  David Rosca <nowrep@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* ============================================================ */
#include "gm_manager.h"
#include "gm_script.h"
#include "gm_downloader.h"
#include "settings/gm_settings.h"

#include "webpage.h"
#include "globalfunctions.h"
#include "mainapplication.h"
#include "desktopnotificationsfactory.h"

#include <QTimer>
#include <QDir>
#include <QWebFrame>
#include <QSettings>
#include <QDebug>

GM_Manager::GM_Manager(const QString &sPath, QObject* parent)
    : QObject(parent)
    , m_settingsPath(sPath)
{
    QTimer::singleShot(0, this, SLOT(load()));
}

void GM_Manager::showSettings(QWidget* parent)
{
    GM_Settings settings(this, parent);
    settings.exec();
}

void GM_Manager::downloadScript(const QNetworkRequest &request)
{
    new GM_Downloader(request, this);
}

QString GM_Manager::settinsPath() const
{
    return m_settingsPath;
}

QString GM_Manager::requireScripts(const QStringList &urlList) const
{
    QDir requiresDir(m_settingsPath + "greasemonkey/requires");
    if (!requiresDir.exists() || urlList.isEmpty()) {
        return QString();
    }

    QSettings settings(m_settingsPath + "greasemonkey/requires/requires.ini", QSettings::IniFormat);
    settings.beginGroup("Files");

    QString script;

    foreach(const QString & url, urlList) {
        if (settings.contains(url)) {
            const QString &fileName = settings.value(url).toString();
            script.append(qz_readAllFileContents(fileName).trimmed() + '\n');
        }
    }

    return script;
}

void GM_Manager::saveSettings()
{
    QSettings settings(m_settingsPath + "extensions.ini", QSettings::IniFormat);
    settings.beginGroup("GreaseMonkey");
    settings.setValue("disabledScripts", m_disabledScripts);
}

QList<GM_Script*> GM_Manager::allScripts() const
{
    QList<GM_Script*> list;
    list.append(m_startScripts);
    list.append(m_endScripts);

    return list;
}

bool GM_Manager::containsScript(const QString &fullName) const
{
    foreach(GM_Script * script, m_startScripts) {
        if (fullName == script->fullName()) {
            return true;
        }
    }

    foreach(GM_Script * script, m_endScripts) {
        if (fullName == script->fullName()) {
            return true;
        }
    }

    return false;

}

void GM_Manager::enableScript(GM_Script* script)
{
    script->setEnabled(true);
    m_disabledScripts.removeOne(script->fullName());
}

void GM_Manager::disableScript(GM_Script* script)
{
    script->setEnabled(false);
    m_disabledScripts.append(script->fullName());
}

bool GM_Manager::addScript(GM_Script* script)
{
    if (!script) {
        return false;
    }

    if (script->startAt() == GM_Script::DocumentStart) {
        m_startScripts.append(script);
    }
    else {
        m_endScripts.append(script);
    }

    emit scriptsChanged();
    return true;
}

bool GM_Manager::removeScript(GM_Script* script)
{
    if (!script) {
        return false;
    }

    if (script->startAt() == GM_Script::DocumentStart) {
        m_startScripts.removeOne(script);
    }
    else {
        m_endScripts.removeOne(script);
    }

    m_disabledScripts.removeOne(script->fullName());
    QFile::remove(script->fileName());
    delete script;

    emit scriptsChanged();
    return true;
}

void GM_Manager::showAddScriptNotification(GM_Script* script) const
{
    QPixmap icon(":gm/data/icon.png");
    QString heading = tr("GreaseMonkey");
    QString text = tr("'%1' installed successfully").arg(script->name());

    mApp->desktopNotifications()->showNotification(icon, heading, text);
}

void GM_Manager::showErrorInstallNotification() const
{
    QPixmap icon(":gm/data/icon.png");
    QString heading = tr("GreaseMonkey Error");
    QString text = tr("Cannot install script");

    mApp->desktopNotifications()->showNotification(icon, heading, text);

    qWarning() << "GreaseMonkey: Cannot install script";
}

void GM_Manager::pageLoadStart()
{
    QWebFrame* frame = qobject_cast<QWebFrame*>(sender());
    if (!frame) {
        return;
    }

    const QString &urlScheme = frame->url().scheme();
    const QString &urlString = frame->url().toEncoded();

    if (!canRunOnScheme(urlScheme)) {
        return;
    }

    foreach(GM_Script * script, m_startScripts) {
        if (script->match(urlString)) {
            frame->evaluateJavaScript(m_bootstrap + script->script());
        }
    }

    foreach(GM_Script * script, m_endScripts) {
        if (script->match(urlString)) {
            const QString &jscript = QString("window.addEventListener(\"DOMContentLoaded\","
                                             "function(e) { %1 }, false);").arg(m_bootstrap + script->script());
            frame->evaluateJavaScript(jscript);
        }
    }
}

void GM_Manager::load()
{
    QDir gmDir(m_settingsPath + "greasemonkey");
    if (!gmDir.exists()) {
        gmDir.mkdir(m_settingsPath + "greasemonkey");
    }

    if (!gmDir.exists("requires")) {
        gmDir.mkdir("requires");
    }

    QSettings settings(m_settingsPath + "extensions.ini", QSettings::IniFormat);
    settings.beginGroup("GreaseMonkey");
    m_disabledScripts = settings.value("disabledScripts", QStringList()).toStringList();

    foreach(const QString & fileName, gmDir.entryList(QStringList("*.js"), QDir::Files)) {
        const QString &absolutePath = gmDir.absoluteFilePath(fileName);
        GM_Script* script = new GM_Script(this, absolutePath);

        if (m_disabledScripts.contains(script->fullName())) {
            script->setEnabled(false);
        }

        if (script->startAt() == GM_Script::DocumentStart) {
            m_startScripts.append(script);
        }
        else {
            m_endScripts.append(script);
        }
    }

    m_bootstrap = qz_readAllFileContents(":gm/data/bootstrap.min.js");
}

bool GM_Manager::canRunOnScheme(const QString &scheme)
{
    return (scheme == "http" || scheme == "https" || scheme == "data" || scheme == "ftp");
}
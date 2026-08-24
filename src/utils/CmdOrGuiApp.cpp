//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include "CmdOrGuiApp.h"
#include "utils/PathHelper.h"
#include <QCoreApplication>
#ifdef __USE_HMI__
  #include "hmi/MainWindow.h"
  #include <QApplication>
#endif

namespace
{
//! Pin the application identity and return the name an older ngPost used for
//! its application-scoped paths.
//!
//! Pinning must run right after the QCoreApplication exists. The GUI also has
//! to adopt the old folder before MainWindow reads ngPost_gui.ini; CLI mode,
//! however, deliberately waits until syntax/help/version/-c have been handled
//! so an inspection command never mutates user files.
QString pinIdentity()
{
    // Read the name Qt derived on its own BEFORE pinning it. That value *is*
    // the directory name every previous ngPost used, on every platform:
    // the argv[0] file name on Unix, the executable base name on Windows,
    // CFBundleName on macOS. Asking Qt is exact; re-deriving those rules here
    // would risk drifting from them.
    const QString derivedName = QCoreApplication::applicationName();

    QCoreApplication::setApplicationName(QStringLiteral("ngPost"));
    return derivedName;
}
} // namespace

CmdOrGuiApp::CmdOrGuiApp(int &argc, char *argv[]):
#ifdef __USE_HMI__
    _app(QCoreApplication::instance()),
    _ownsApp(_app == nullptr),
    _derivedApplicationName(),
    _mode(argc > 1 ? AppMode::CMD : AppMode::HMI),
    _hmi(nullptr)
#else
    _app(QCoreApplication::instance()),
    _ownsApp(_app == nullptr),
    _derivedApplicationName()
#endif
{
#ifdef __USE_HMI__
    if (!_app) {
        if (_mode == AppMode::CMD)
            _app = new QCoreApplication(argc, argv);
        else
            _app = new QApplication(argc, argv);
    }
    _derivedApplicationName = pinIdentity();
    if (_mode == AppMode::HMI) {
        // Before MainWindow: it reads ngPost_gui.ini out of configDir().
        PathHelper::migrateAppNamedConfigDirIfNeeded(_derivedApplicationName);
        _hmi = new MainWindow();
    }
#else
    if (!_app)
        _app = new QCoreApplication(argc, argv);
    _derivedApplicationName = pinIdentity();
#endif
}

CmdOrGuiApp::~CmdOrGuiApp()
{
#ifdef __USE_HMI__
    if (_hmi)
        delete  _hmi;
#endif
    if (_ownsApp)
        delete _app;
}

void CmdOrGuiApp::checkForNewVersion()
{
    // No need here
    // check ngPost implementation if required
    // https://github.com/Hydro74000/ngPost
}

int CmdOrGuiApp::startEventLoop() { return _app->exec(); }

#ifdef __USE_HMI__
int CmdOrGuiApp::startHMI()
{
    _hmi->show();
    return _app->exec();
}

void CmdOrGuiApp::hideOrShowGUI()
{
    if (_hmi)
    {
        if (_hmi->isVisible())
            _hmi->hide();
        else
            _hmi->show();
    }
}
#endif

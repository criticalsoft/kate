/* This file is part of the KDE project
   Copyright (C) 2001 Christoph Cullmann <cullmann@kde.org>
   Copyright (C) 2002 Joseph Wenninger <jowenn@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "kateapp.h"
#include "kateapp.moc"

#include "katedocmanager.h"
#include "katepluginmanager.h"
#include "kateviewmanager.h"
#include "katesession.h"
#include "katemainwindow.h"
#include "kateappcommands.h"
#include "katedebug.h"

#include <kate/application.h>

#include <KConfig>
#include <ktip.h>
#include <KMessageBox>
#include <KStartupInfo>
#include <KLocalizedString>
#include <kconfiggui.h>
#include <KConfigGroup>

#include <QCommandLineParser>
#include <QFileInfo>
#include <QTextCodec>
#include <QApplication>

#include "kateappadaptor.h"

KateApp *KateApp::s_self = 0;

Q_LOGGING_CATEGORY(LOG_KATE, "kate")

KateApp::KateApp(const QCommandLineParser &args)
    : m_args (args)
    , m_wrapper (new KTextEditor::Application (this))
{
  s_self = this;

  // application interface
  m_application = new Kate::Application (this);

  // doc man
  m_docManager = new KateDocManager (this);

  // init all normal plugins
  m_pluginManager = new KatePluginManager (this);

  // session manager up
  m_sessionManager = new KateSessionManager (this);
  
  // dbus
  m_adaptor = new KateAppAdaptor( this );

  m_appCommands = KateAppCommands::self();
}

KateApp::~KateApp ()
{  
  // unregister...
  m_adaptor->emitExiting ();
  QDBusConnection::sessionBus().unregisterObject( QLatin1String("/MainApplication") );
  delete m_adaptor;
  
  // l8r, app commands
  delete m_appCommands;

  // cu session manager
  delete m_sessionManager;

  // cu plugin manager
  delete m_pluginManager;

  // delete this now, or we crash
  delete m_docManager;

  // cu kate app
  delete m_application;
}

KateApp *KateApp::self ()
{
  return s_self;
}

Kate::Application *KateApp::application ()
{
  return m_application;
}

bool KateApp::init ()
{

  qCDebug(LOG_KATE) << "Setting KATE_PID: '" << QCoreApplication::applicationPid() << "'";
  ::setenv( "KATE_PID", QString("%1").arg(QCoreApplication::applicationPid()).toLatin1().constData(), 1 );

  // handle restore different
  if (qApp->isSessionRestored())
  {
    restoreKate ();
  }
  else
  {
    // let us handle our command line args and co ;)
    // we can exit here if session chooser decides
    if (!startupKate ())
    {
      qCDebug(LOG_KATE) << "startupKate returned false";
      return false;
    }
  }

  // application dbus interface
  QDBusConnection::sessionBus().registerObject( QLatin1String("/MainApplication"), this );
  return true;
}

void KateApp::restoreKate ()
{
  KConfig *sessionConfig = KConfigGui::sessionConfig();
  
  // activate again correct session!!!
  QString lastSession (sessionConfig->group("General").readEntry ("Last Session", QString()));
  sessionManager()->activateSession (KateSession::Ptr(new KateSession (sessionManager(), lastSession)), false, false, false);

  // plugins
  KatePluginManager::self ()->loadConfig (sessionConfig);

  // restore the files we need
  m_docManager->restoreDocumentList (sessionConfig);

  // restore all windows ;)
  for (int n = 1; KMainWindow::canBeRestored(n); n++)
    newMainWindow(sessionConfig, QString ("%1").arg(n));
  
  // oh, no mainwindow, create one, should not happen, but make sure ;)
  if (mainWindowsCount() == 0)
    newMainWindow ();
}

bool KateApp::startupKate ()
{
  // user specified session to open
  if (m_args.isSet ("startanon"))
  {
    sessionManager()->activateSession (sessionManager()->giveSession (""), false, false);
  }
  else  if (m_args.isSet ("start"))
  {
    sessionManager()->activateSession (sessionManager()->giveSession (m_args.value("start")), false, false);
  }
  else if (!m_args.isSet( "stdin" ) && (m_args.positionalArguments().count() == 0)) // only start session if no files specified
  {
    // let the user choose session if possible
    if (!sessionManager()->chooseSession ())
    {
      qCDebug(LOG_KATE) << "chooseSession returned false, exiting";
      // we will exit kate now, notify the rest of the world we are done
#ifdef Q_WS_X11
      KStartupInfo::appStarted (startupId());
#endif
      return false;
    }
  }
  else
  {
    sessionManager()->activateSession( KateSession::Ptr(new KateSession (sessionManager(), QString())), false, false );
  }
  
  // oh, no mainwindow, create one, should not happen, but make sure ;)
  if (mainWindowsCount() == 0)
    newMainWindow ();

  // notify about start
#ifdef Q_WS_X11
  KStartupInfo::setNewStartupId( activeKateMainWindow(), startupId());
#endif
  
  QTextCodec *codec = m_args.isSet("encoding") ? QTextCodec::codecForName(m_args.value("encoding").toUtf8()) : 0;
  bool tempfileSet = m_args.isSet("tempfile");

  KTextEditor::Document *doc = 0;
  KateDocManager::self()->setSuppressOpeningErrorDialogs(true);
  Q_FOREACH (const QString positionalArgument, m_args.positionalArguments())
  {
    QUrl url;

    // convert to an url
    QRegExp withProtocol("^[a-zA-Z]+:"); // TODO: remove after Qt supports this on its own
    if (withProtocol.indexIn(positionalArgument) == 0) {
      url = QUrl::fromUserInput(positionalArgument);
    } else {
      url = QUrl::fromLocalFile(positionalArgument);
    }

    // this file is no local dir, open it, else warn
    bool noDir = !url.isLocalFile() || !QFileInfo (url.toLocalFile()).isDir();

    if (noDir)
    {
      // open a normal file
      if (codec)
        doc = activeKateMainWindow()->viewManager()->openUrl( url, codec->name(), false, tempfileSet);
      else
        doc = activeKateMainWindow()->viewManager()->openUrl( url, QString(), false, tempfileSet);
    }
    else
      KMessageBox::sorry( activeKateMainWindow(),
                          i18n("The file '%1' could not be opened: it is not a normal file, it is a folder.", url.toString()) );
  }
  KateDocManager::self()->setSuppressOpeningErrorDialogs(false);

  // handle stdin input
  if( m_args.isSet( "stdin" ) )
  {
    QTextStream input(stdin, QIODevice::ReadOnly);

    // set chosen codec
    if (codec)
      input.setCodec (codec);

    QString line;
    QString text;

    do
    {
      line = input.readLine();
      text.append( line + '\n' );
    }
    while( !line.isNull() );

    openInput (text);
  }
  else if ( doc )
    activeKateMainWindow()->viewManager()->activateView( doc );

  if ( activeKateMainWindow()->viewManager()->viewCount () == 0 )
    activeKateMainWindow()->viewManager()->activateView(m_docManager->document (0));

  int line = 0;
  int column = 0;
  bool nav = false;

  if (m_args.isSet ("line"))
  {
    line = m_args.value ("line").toInt() - 1;
    nav = true;
  }

  if (m_args.isSet ("column"))
  {
    column = m_args.value ("column").toInt() - 1;
    nav = true;
  }

  if (nav && activeKateMainWindow()->viewManager()->activeView ())
    activeKateMainWindow()->viewManager()->activeView ()->setCursorPosition (KTextEditor::Cursor (line, column));
  
  // show the nice tips
  KTipDialog::showTip(activeKateMainWindow());

  activeKateMainWindow()->setAutoSaveSettings();

  qCDebug(LOG_KATE) << "KateApplication::init finished successful";
  return true;
}

void KateApp::shutdownKate (KateMainWindow *win)
{
  if (!win->queryClose_internal())
    return;

  sessionManager()->saveActiveSession(true);

  // cu main windows
  while (!m_mainWindows.isEmpty())
  {
    // mainwindow itself calls KateApp::removeMainWindow(this)
    delete m_mainWindows[0];
  }

  QApplication::quit ();
}

KatePluginManager *KateApp::pluginManager()
{
  return m_pluginManager;
}

KateDocManager *KateApp::documentManager ()
{
  return m_docManager;
}

KateSessionManager *KateApp::sessionManager ()
{
  return m_sessionManager;
}

bool KateApp::openUrl (const QUrl &url, const QString &encoding, bool isTempFile)
{
  return openDocUrl(url,encoding,isTempFile);
}

KTextEditor::Document* KateApp::openDocUrl (const QUrl &url, const QString &encoding, bool isTempFile)
{
  KateMainWindow *mainWindow = activeKateMainWindow ();

  if (!mainWindow)
    return 0;

  QTextCodec *codec = encoding.isEmpty() ? 0 : QTextCodec::codecForName(encoding.toLatin1());

  // this file is no local dir, open it, else warn
  bool noDir = !url.isLocalFile() || !QFileInfo (url.toLocalFile()).isDir();

  KTextEditor::Document *doc=0;
  
  if (noDir)
  {
    // show no errors...
    documentManager()->setSuppressOpeningErrorDialogs (true);

    // open a normal file
    if (codec)
      doc=mainWindow->viewManager()->openUrl( url, codec->name(), true, isTempFile);
    else
      doc=mainWindow->viewManager()->openUrl( url, QString(), true, isTempFile );
    
    // back to normal....
    documentManager()->setSuppressOpeningErrorDialogs (false);
  }
  else
    KMessageBox::sorry( mainWindow,
                        i18n("The file '%1' could not be opened: it is not a normal file, it is a folder.", url.url()) );

  return doc;
}

bool KateApp::setCursor (int line, int column)
{
  KateMainWindow *mainWindow = activeKateMainWindow ();

  if (!mainWindow)
    return false;

  if (mainWindow->viewManager()->activeView ())
    mainWindow->viewManager()->activeView ()->setCursorPosition (KTextEditor::Cursor (line, column));

  return true;
}

bool KateApp::openInput (const QString &text)
{
  activeKateMainWindow()->viewManager()->openUrl( QUrl(), "", true );

  if (!activeKateMainWindow()->viewManager()->activeView ())
    return false;

  KTextEditor::Document *doc = activeKateMainWindow()->viewManager()->activeView ()->document();

  if (!doc)
    return false;

  return doc->setText (text);
}

KateMainWindow *KateApp::newMainWindow (KConfig *sconfig_, const QString &sgroup_)
{
  KConfig *sconfig = sconfig_ ? sconfig_ : KSharedConfig::openConfig().data();
  QString sgroup = !sgroup_.isEmpty() ? sgroup_ : "MainWindow0";

  KateMainWindow *mainWindow = new KateMainWindow (sconfig, sgroup);
  mainWindow->show ();

  return mainWindow;
}

void KateApp::addMainWindow (KateMainWindow *mainWindow)
{
  m_mainWindows.push_back (mainWindow);
  m_mainWindowsInterfaces.push_back (mainWindow->mainWindow());
}

void KateApp::removeMainWindow (KateMainWindow *mainWindow)
{
  m_mainWindowsInterfaces.removeAll(mainWindow->mainWindow());
  m_mainWindows.removeAll(mainWindow);
}

KateMainWindow *KateApp::activeKateMainWindow ()
{
  if (m_mainWindows.isEmpty())
    return 0;

  int n = m_mainWindows.indexOf (static_cast<KateMainWindow *>((static_cast<QApplication *>(QCoreApplication::instance ())->activeWindow())));

  if (n < 0)
    n = 0;

  return m_mainWindows[n];
}

int KateApp::mainWindowsCount () const
{
  return m_mainWindows.size();
}

int KateApp::mainWindowID(KateMainWindow *window)
{
  for (int i = 0;i < m_mainWindows.size();i++)
    if (window == m_mainWindows[i]) return i;
  return -1;
}

KateMainWindow *KateApp::mainWindow (int n)
{
  if (n < m_mainWindows.size())
    return m_mainWindows[n];

  return 0;
}

void KateApp::emitDocumentClosed(const QString& token)
{
  m_adaptor->emitDocumentClosed(token);
}

// kate: space-indent on; indent-width 2; replace-tabs on;

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

#ifndef __KATE_APP_H__
#define __KATE_APP_H__

#include <kateinterfaces_export.h>
#include <kate/mainwindow.h>
#include <ktexteditor/application.h>

#include <katemainwindow.h>

#include <KConfig>
#include <QList>

class KateSessionManager;
class KateMainWindow;
class KatePluginManager;
class KateDocManager;
class KateAppCommands;
class KateAppAdaptor;
class QCommandLineParser;

namespace KTextEditor
{
  class Document;
}

namespace Kate
{
  class Application;
}

/**
 * Kate Application
 * This class represents the core kate application object
 */
class KATEINTERFACES_EXPORT KateApp : public QObject
{
    Q_OBJECT

    /**
     * constructors & accessor to app object + plugin interface for it
     */
  public:
    /**
     * application constructor
     */
    KateApp (const QCommandLineParser &args);
    
    /**
     * get kate inited
     * @return false, if application should exit
     */
    bool init ();

    /**
     * application destructor
     */
    ~KateApp ();

    /**
     * static accessor to avoid casting ;)
     * @return app instance
     */
    static KateApp *self ();

    /**
     * accessor to the Kate::Application plugin interface
     * @return application plugin interface
     */
    Kate::Application *application ();
    
    /**
     * KTextEditor::Application wrapper
     * @return KTextEditor::Application wrapper.
     */
    KTextEditor::Application *wrapper ()
    {
      return m_wrapper;
    }

    /**
     * kate init
     */
  private:
    /**
     * restore a old kate session
     */
    void restoreKate ();

    /**
     * try to start kate
     * @return success, if false, kate should exit
     */
    bool startupKate ();

    /**
     * kate shutdown
     */
  public:
    /**
     * shutdown kate application
     * @param win mainwindow which is used for dialogs
     */
    void shutdownKate (KateMainWindow *win);

    /**
     * other accessors for global unique instances
     */
  public:
    /**
     * accessor to plugin manager
     * @return plugin manager instance
     */
    KatePluginManager *pluginManager();

    /**
     * accessor to document manager
     * @return document manager instance
     */
    KateDocManager *documentManager ();

    /**
     * accessor to session manager
     * @return session manager instance
     */
    KateSessionManager *sessionManager ();

    /**
     * window management
     */
  public:
    /**
     * create a new main window, use given config if any for restore
     * @param sconfig session config object
     * @param sgroup session group for this window
     * @return new constructed main window
     */
    KateMainWindow *newMainWindow (KConfig *sconfig = 0, const QString &sgroup = "");

    /**
     * add the mainwindow given
     * should be called in mainwindow constructor
     * @param mainWindow window to remove
     */
    void addMainWindow (KateMainWindow *mainWindow);

    /**
     * removes the mainwindow given, DOES NOT DELETE IT
     * should be called in mainwindow destructor
     * @param mainWindow window to remove
     */
    void removeMainWindow (KateMainWindow *mainWindow);

    /**
     * give back current active main window
     * can only be 0 at app start or exit
     * @return current active main window
     */
    KateMainWindow *activeKateMainWindow ();

    /**
     * give back number of existing main windows
     * @return number of main windows
     */
    int mainWindowsCount () const;

    /**
     * give back the window you want
     * @param n window index
     * @return requested main window
     */
    KateMainWindow *mainWindow (int n);

    int mainWindowID(KateMainWindow *window);

    /**
     * some stuff for the dcop API
     */
  public:
    /**
     * open url with given encoding
     * used by kate if --use given
     * @param url filename
     * @param encoding encoding name
     * @return success
     */
    bool openUrl (const QUrl &url, const QString &encoding, bool isTempFile);

    KTextEditor::Document* openDocUrl (const QUrl &url, const QString &encoding, bool isTempFile);
    
    void emitDocumentClosed(const QString& token);
    
    /**
     * position cursor in current active view
     * @param line line to set
     * @param column column to set
     * @return success
     */
    bool setCursor (int line, int column);

    /**
     * helper to handle stdin input
     * open a new document/view, fill it with the text given
     * @param text text to fill in the new doc/view
     * @return success
     */
    bool openInput (const QString &text);

    /**
     * Get a list of all mainwindows interfaces for the plugins.
     * @return all mainwindows
     * @see activeMainWindow()
     */
    const QList<Kate::MainWindow*> &mainWindowsInterfaces () const
    {
      return m_mainWindowsInterfaces;
    }
  
  //
  // KTextEditor::Application interface, called by wrappers via invokeMethod
  //
  public Q_SLOTS:
    /**
     * Get a list of all main windows.
     * @return all main windows
     */
    QList<KTextEditor::MainWindow *> mainWindows ()
    {
      // assemble right list
      QList<KTextEditor::MainWindow *> windows;
      for (int i = 0; i < m_mainWindows.size(); ++i)
        windows.push_back (m_mainWindows[i]->wrapper());
      return windows;
    }
    
    /**
     * Accessor to the active main window.
     * \return a pointer to the active mainwindow
     */
    KTextEditor::MainWindow *activeMainWindow ()
    {
      // either return wrapper or nullptr
      if (KateMainWindow *a = activeKateMainWindow ())
        return a->wrapper ();
      return nullptr;
    }

  private:
    /**
     * Singleton instance
     */
    static KateApp *s_self;
    
    /**
     * kate's command line args
     */
    const QCommandLineParser &m_args;

    /**
     * plugin interface
     */
    Kate::Application *m_application;

    /**
     * document manager
     */
    KateDocManager *m_docManager;

    /**
     * plugin manager
     */
    KatePluginManager *m_pluginManager;

    /**
     * session manager
     */
    KateSessionManager *m_sessionManager;

    /**
     * dbus interface
     */
    KateAppAdaptor *m_adaptor;

    /**
     * known main windows
     */
    QList<KateMainWindow*> m_mainWindows;
    QList<Kate::MainWindow*> m_mainWindowsInterfaces;

    // various vim-inspired command line commands
    KateAppCommands *m_appCommands;
    
    /**
     * Wrapper of application for KTextEditor
     */
    KTextEditor::Application *m_wrapper;

};

#endif
// kate: space-indent on; indent-width 2; replace-tabs on;


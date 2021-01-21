/*  SPDX-License-Identifier: MIT

    SPDX-FileCopyrightText: 2019 Mark Nauwelaerts <mark.nauwelaerts@gmail.com>

    SPDX-License-Identifier: MIT
*/

#include "lspclientpluginview.h"
#include "lspclientcompletion.h"
#include "lspclienthover.h"
#include "lspclientplugin.h"
#include "lspclientservermanager.h"
#include "lspclientsymbolview.h"

#include "lspclient_debug.h"

#include <KAcceleratorManager>
#include <KActionCollection>
#include <KActionMenu>
#include <KLocalizedString>
#include <KStandardAction>
#include <KXMLGUIFactory>

#include <KTextEditor/CodeCompletionInterface>
#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Message>
#include <KTextEditor/MovingInterface>
#include <KTextEditor/View>
#include <KXMLGUIClient>

#include <ktexteditor/configinterface.h>
#include <ktexteditor/markinterface.h>
#include <ktexteditor/movinginterface.h>
#include <ktexteditor/movingrange.h>
#include <ktexteditor_version.h>

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSet>
#include <QStandardItem>
#include <QTextCodec>
#include <QTimer>
#include <QTreeView>
#include <utility>

namespace RangeData
{
enum {
    // preserve UserRole for generic use where needed
    FileUrlRole = Qt::UserRole + 1,
    RangeRole,
    KindRole,
};

class KindEnum
{
public:
    enum _kind {
        Text = static_cast<int>(LSPDocumentHighlightKind::Text),
        Read = static_cast<int>(LSPDocumentHighlightKind::Read),
        Write = static_cast<int>(LSPDocumentHighlightKind::Write),
        Error = 10 + static_cast<int>(LSPDiagnosticSeverity::Error),
        Warning = 10 + static_cast<int>(LSPDiagnosticSeverity::Warning),
        Information = 10 + static_cast<int>(LSPDiagnosticSeverity::Information),
        Hint = 10 + static_cast<int>(LSPDiagnosticSeverity::Hint),
        Related
    };

    KindEnum(int v)
    {
        m_value = _kind(v);
    }

    KindEnum(LSPDocumentHighlightKind hl)
        : KindEnum(static_cast<_kind>(hl))
    {
    }

    KindEnum(LSPDiagnosticSeverity sev)
        : KindEnum(_kind(10 + static_cast<int>(sev)))
    {
    }

    operator _kind()
    {
        return m_value;
    }

private:
    _kind m_value;
};

static constexpr KTextEditor::MarkInterface::MarkTypes markType = KTextEditor::MarkInterface::markType31;
static constexpr KTextEditor::MarkInterface::MarkTypes markTypeDiagError = KTextEditor::MarkInterface::Error;
static constexpr KTextEditor::MarkInterface::MarkTypes markTypeDiagWarning = KTextEditor::MarkInterface::Warning;
static constexpr KTextEditor::MarkInterface::MarkTypes markTypeDiagOther = KTextEditor::MarkInterface::markType30;
static constexpr KTextEditor::MarkInterface::MarkTypes markTypeDiagAll = KTextEditor::MarkInterface::MarkTypes(markTypeDiagError | markTypeDiagWarning | markTypeDiagOther);

}

static QIcon diagnosticsIcon(LSPDiagnosticSeverity severity)
{
    // clang-format off
#define RETURN_CACHED_ICON(name, fallbackname) \
    { \
        static QIcon icon(QIcon::fromTheme(QStringLiteral(name), \
                                           QIcon::fromTheme(QStringLiteral(fallbackname)))); \
        return icon; \
    }
    // clang-format on
    switch (severity) {
    case LSPDiagnosticSeverity::Error:
        RETURN_CACHED_ICON("data-error", "dialog-error")
    case LSPDiagnosticSeverity::Warning:
        RETURN_CACHED_ICON("data-warning", "dialog-warning")
    case LSPDiagnosticSeverity::Information:
    case LSPDiagnosticSeverity::Hint:
        RETURN_CACHED_ICON("data-information", "dialog-information")
    default:
        break;
    }
    return QIcon();
}

static QIcon codeActionIcon()
{
    static QIcon icon(QIcon::fromTheme(QStringLiteral("insert-text")));
    return icon;
}

KTextEditor::Document *findDocument(KTextEditor::MainWindow *mainWindow, const QUrl &url)
{
    auto views = mainWindow->views();
    for (const auto v : views) {
        auto doc = v->document();
        if (doc && doc->url() == url)
            return doc;
    }
    return nullptr;
}

// helper to read lines from unopened documents
// lightweight and does not require additional symbols
class FileLineReader
{
    QFile file;
    int lastLineNo = -1;
    QString lastLine;

public:
    FileLineReader(const QUrl &url)
        : file(url.toLocalFile())
    {
        file.open(QIODevice::ReadOnly);
    }

    // called with non-descending lineno
    QString line(int lineno)
    {
        if (lineno == lastLineNo) {
            return lastLine;
        }
        while (file.isOpen() && !file.atEnd()) {
            auto line = file.readLine();
            if (++lastLineNo == lineno) {
                QTextCodec::ConverterState state;
                QTextCodec *codec = QTextCodec::codecForName("UTF-8");
                QString text = codec->toUnicode(line.constData(), line.size(), &state);
                if (state.invalidChars > 0) {
                    text = QString::fromLatin1(line);
                }
                while (text.size() && text.at(text.size() - 1).isSpace())
                    text.chop(1);
                lastLine = text;
                return text;
            }
        }
        return QString();
    }
};


/**
 * @brief This is just a helper class that provides "underline" on Ctrl + click
 */
class CtrlHoverFeedback : public QObject
{
    Q_OBJECT
public:

    void highlight(KTextEditor::View* activeView)
    {
        // sanity checks
        if (!activeView)
            return;

        auto doc = activeView->document();
        if (!doc)
            return;

        // set the cursor
        if (w)
            w->setCursor(Qt::PointingHandCursor);

        // underline the hovered word
        auto mr = ranges[doc];
        if (mr) {
            mr->setRange(range);
        } else {
            auto miface = qobject_cast<KTextEditor::MovingInterface*>(doc);
            if (!miface)
                return;
            mr = miface->newMovingRange(range);
            ranges[doc] = mr;
            connect(doc, SIGNAL(aboutToInvalidateMovingInterfaceContent(KTextEditor::Document *)), this, SLOT(clear(KTextEditor::Document *)), Qt::UniqueConnection);
            connect(doc, SIGNAL(aboutToDeleteMovingInterfaceContent(KTextEditor::Document *)), this, SLOT(clear(KTextEditor::Document *)), Qt::UniqueConnection);
        }

        static KTextEditor::Attribute::Ptr attr;
        if (!attr) {
            attr = new KTextEditor::Attribute;
            attr->setUnderlineStyle(QTextCharFormat::SingleUnderline);
        }
        mr->setAttribute(attr);
    }

    void clear(KTextEditor::View* activeView)
    {
        if (activeView) {
            auto doc = activeView->document();
            if (doc) {
                auto& mr = ranges[doc];
                if (mr)
                    mr->setRange(KTextEditor::Range::invalid());
            }
        }
    }

    void setRangeAndWidget(const KTextEditor::Range& r, QWidget* wid)
    {
        range = r;
        w = wid;
    }

    bool isValid() const
    {
        return w != nullptr;
    }

private:

    Q_SLOT void clear(KTextEditor::Document* doc)
    {
        if (doc) {
            auto it = ranges.find(doc);
            if (it != ranges.end()) {
                delete *it;
                ranges.erase(it);
            }
        }
    }

private:
    QWidget* w = nullptr;
    QHash<KTextEditor::Document*, KTextEditor::MovingRange*> ranges;
    KTextEditor::Range range;
};

class LSPClientActionView : public QObject
{
    Q_OBJECT

    typedef LSPClientActionView self_type;

    LSPClientPlugin *m_plugin;
    KTextEditor::MainWindow *m_mainWindow;
    KXMLGUIClient *m_client;
    QSharedPointer<LSPClientServerManager> m_serverManager;
    QScopedPointer<LSPClientViewTracker> m_viewTracker;
    QScopedPointer<LSPClientCompletion> m_completion;
    QScopedPointer<LSPClientHover> m_hover;
    QScopedPointer<KTextEditor::TextHintProvider> m_forwardHover;
    QScopedPointer<QObject> m_symbolView;

    QPointer<QAction> m_findDef;
    QPointer<QAction> m_findDecl;
    QPointer<QAction> m_findRef;
    QPointer<QAction> m_findImpl;
    QPointer<QAction> m_triggerHighlight;
    QPointer<QAction> m_triggerSymbolInfo;
    QPointer<QAction> m_triggerFormat;
    QPointer<QAction> m_triggerRename;
    QPointer<QAction> m_complDocOn;
    QPointer<QAction> m_refDeclaration;
    QPointer<QAction> m_autoHover;
    QPointer<QAction> m_onTypeFormatting;
    QPointer<QAction> m_incrementalSync;
    QPointer<QAction> m_diagnostics;
    QPointer<QAction> m_diagnosticsHighlight;
    QPointer<QAction> m_diagnosticsMark;
    QPointer<QAction> m_diagnosticsHover;
    QPointer<QAction> m_diagnosticsSwitch;
    QPointer<QAction> m_messages;
    QPointer<KSelectAction> m_messagesAutoSwitch;
    QPointer<QAction> m_messagesSwitch;
    QPointer<QAction> m_closeDynamic;
    QPointer<QAction> m_restartServer;
    QPointer<QAction> m_restartAll;

    // toolview
    QScopedPointer<QWidget> m_toolView;
    QPointer<QTabWidget> m_tabWidget;
    // applied search ranges
    typedef QMultiHash<KTextEditor::Document *, KTextEditor::MovingRange *> RangeCollection;
    RangeCollection m_ranges;
    QHash<KTextEditor::Document *, QHash<int, QVector<KTextEditor::MovingRange *>>> m_semanticHighlightRanges;
    // applied marks
    typedef QSet<KTextEditor::Document *> DocumentCollection;
    DocumentCollection m_marks;
    // modelis either owned by tree added to tabwidget or owned here
    QScopedPointer<QStandardItemModel> m_ownedModel;
    // in either case, the model that directs applying marks/ranges
    QPointer<QStandardItemModel> m_markModel;
    // goto definition and declaration jump list is more a menu than a
    // search result, so let's not keep adding new tabs for those
    // previous tree for definition result
    QPointer<QTreeView> m_defTree;
    // ... and for declaration
    QPointer<QTreeView> m_declTree;

    // diagnostics tab
    QPointer<QTreeView> m_diagnosticsTree;
    // tree widget is either owned here or by tab
    QScopedPointer<QTreeView> m_diagnosticsTreeOwn;
    QScopedPointer<QStandardItemModel> m_diagnosticsModel;
    // diagnostics ranges
    RangeCollection m_diagnosticsRanges;
    // and marks
    DocumentCollection m_diagnosticsMarks;

    using MessagesWidget = QPlainTextEdit;
    // messages tab
    QPointer<MessagesWidget> m_messagesView;
    // widget is either owned here or by tab
    QScopedPointer<MessagesWidget> m_messagesViewOwn;

    // views on which completions have been registered
    QSet<KTextEditor::View *> m_completionViews;

    // views on which hovers have been registered
    QSet<KTextEditor::View *> m_hoverViews;

    // outstanding request
    LSPClientServer::RequestHandle m_handle;
    // timeout on request
    bool m_req_timeout = false;

    // accept incoming applyEdit
    bool m_accept_edit = false;
    // characters to trigger format request
    QVector<QChar> m_onTypeFormattingTriggers;

    CtrlHoverFeedback m_ctrlHoverFeedback = {};

    KActionCollection *actionCollection() const
    {
        return m_client->actionCollection();
    }

    // inner class that forwards directly to method for convenience
    class ForwardingTextHintProvider : public KTextEditor::TextHintProvider
    {
        LSPClientActionView *m_parent;

    public:
        ForwardingTextHintProvider(LSPClientActionView *parent)
            : m_parent(parent)
        {
            Q_ASSERT(m_parent);
        }

        virtual QString textHint(KTextEditor::View *view, const KTextEditor::Cursor &position) override
        {
            return m_parent->onTextHint(view, position);
        }
    };

public:
    LSPClientActionView(LSPClientPlugin *plugin, KTextEditor::MainWindow *mainWin, KXMLGUIClient *client, QSharedPointer<LSPClientServerManager> serverManager)
        : QObject(mainWin)
        , m_plugin(plugin)
        , m_mainWindow(mainWin)
        , m_client(client)
        , m_serverManager(std::move(serverManager))
        , m_completion(LSPClientCompletion::new_(m_serverManager))
        , m_hover(LSPClientHover::new_(m_serverManager))
        , m_forwardHover(new ForwardingTextHintProvider(this))
        , m_symbolView(LSPClientSymbolView::new_(plugin, mainWin, m_serverManager))
    {
        connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &self_type::updateState);
        connect(m_mainWindow, &KTextEditor::MainWindow::unhandledShortcutOverride, this, &self_type::handleEsc);
        connect(m_serverManager.data(), &LSPClientServerManager::serverChanged, this, &self_type::updateState);
        connect(m_serverManager.data(), &LSPClientServerManager::showMessage, this, &self_type::onShowMessage);

        m_findDef = actionCollection()->addAction(QStringLiteral("lspclient_find_definition"), this, &self_type::goToDefinition);
        m_findDef->setText(i18n("Go to Definition"));
        m_findDecl = actionCollection()->addAction(QStringLiteral("lspclient_find_declaration"), this, &self_type::goToDeclaration);
        m_findDecl->setText(i18n("Go to Declaration"));
        m_findRef = actionCollection()->addAction(QStringLiteral("lspclient_find_references"), this, &self_type::findReferences);
        m_findRef->setText(i18n("Find References"));
        m_findImpl = actionCollection()->addAction(QStringLiteral("lspclient_find_implementations"), this, &self_type::findImplementation);
        m_findImpl->setText(i18n("Find Implementations"));
        m_triggerHighlight = actionCollection()->addAction(QStringLiteral("lspclient_highlight"), this, &self_type::highlight);
        m_triggerHighlight->setText(i18n("Highlight"));
        m_triggerSymbolInfo = actionCollection()->addAction(QStringLiteral("lspclient_symbol_info"), this, &self_type::symbolInfo);
        m_triggerSymbolInfo->setText(i18n("Symbol Info"));
        m_triggerFormat = actionCollection()->addAction(QStringLiteral("lspclient_format"), this, &self_type::format);
        m_triggerFormat->setText(i18n("Format"));
        m_triggerRename = actionCollection()->addAction(QStringLiteral("lspclient_rename"), this, &self_type::rename);
        m_triggerRename->setText(i18n("Rename"));

        // general options
        m_complDocOn = actionCollection()->addAction(QStringLiteral("lspclient_completion_doc"), this, &self_type::displayOptionChanged);
        m_complDocOn->setText(i18n("Show selected completion documentation"));
        m_complDocOn->setCheckable(true);
        m_refDeclaration = actionCollection()->addAction(QStringLiteral("lspclient_references_declaration"), this, &self_type::displayOptionChanged);
        m_refDeclaration->setText(i18n("Include declaration in references"));
        m_refDeclaration->setCheckable(true);
        m_autoHover = actionCollection()->addAction(QStringLiteral("lspclient_auto_hover"), this, &self_type::displayOptionChanged);
        m_autoHover->setText(i18n("Show hover information"));
        m_autoHover->setCheckable(true);
        m_onTypeFormatting = actionCollection()->addAction(QStringLiteral("lspclient_type_formatting"), this, &self_type::displayOptionChanged);
        m_onTypeFormatting->setText(i18n("Format on typing"));
        m_onTypeFormatting->setCheckable(true);
        m_incrementalSync = actionCollection()->addAction(QStringLiteral("lspclient_incremental_sync"), this, &self_type::displayOptionChanged);
        m_incrementalSync->setText(i18n("Incremental document synchronization"));
        m_incrementalSync->setCheckable(true);

        // diagnostics
        m_diagnostics = actionCollection()->addAction(QStringLiteral("lspclient_diagnostics"), this, &self_type::displayOptionChanged);
        m_diagnostics->setText(i18n("Show diagnostics notifications"));
        m_diagnostics->setCheckable(true);
        m_diagnosticsHighlight = actionCollection()->addAction(QStringLiteral("lspclient_diagnostics_highlight"), this, &self_type::displayOptionChanged);
        m_diagnosticsHighlight->setText(i18n("Show diagnostics highlights"));
        m_diagnosticsHighlight->setCheckable(true);
        m_diagnosticsMark = actionCollection()->addAction(QStringLiteral("lspclient_diagnostics_mark"), this, &self_type::displayOptionChanged);
        m_diagnosticsMark->setText(i18n("Show diagnostics marks"));
        m_diagnosticsMark->setCheckable(true);
        m_diagnosticsHover = actionCollection()->addAction(QStringLiteral("lspclient_diagnostics_hover"), this, &self_type::displayOptionChanged);
        m_diagnosticsHover->setText(i18n("Show diagnostics on hover"));
        m_diagnosticsHover->setCheckable(true);
        m_diagnosticsSwitch = actionCollection()->addAction(QStringLiteral("lspclient_diagnostic_switch"), this, &self_type::switchToDiagnostics);
        m_diagnosticsSwitch->setText(i18n("Switch to diagnostics tab"));

        // messages
        m_messages = actionCollection()->addAction(QStringLiteral("lspclient_messages"), this, &self_type::displayOptionChanged);
        m_messages->setText(i18n("Show messages"));
        m_messages->setCheckable(true);
        m_messagesAutoSwitch = new KSelectAction(i18n("Switch to messages tab upon message level"), this);
        actionCollection()->addAction(QStringLiteral("lspclient_messages_auto_switch"), m_messagesAutoSwitch);
        const QStringList list {i18nc("@info", "Never"), i18nc("@info", "Error"), i18nc("@info", "Warning"), i18nc("@info", "Information"), i18nc("@info", "Log")};
        m_messagesAutoSwitch->setItems(list);
        m_messagesSwitch = actionCollection()->addAction(QStringLiteral("lspclient_messages_switch"), this, &self_type::switchToMessages);
        m_messagesSwitch->setText(i18n("Switch to messages tab"));

        // server control and misc actions
        m_closeDynamic = actionCollection()->addAction(QStringLiteral("lspclient_close_dynamic"), this, &self_type::closeDynamic);
        m_closeDynamic->setText(i18n("Close all dynamic reference tabs"));
        m_restartServer = actionCollection()->addAction(QStringLiteral("lspclient_restart_server"), this, &self_type::restartCurrent);
        m_restartServer->setText(i18n("Restart LSP Server"));
        m_restartAll = actionCollection()->addAction(QStringLiteral("lspclient_restart_all"), this, &self_type::restartAll);
        m_restartAll->setText(i18n("Restart All LSP Servers"));

        // popup menu
        auto menu = new KActionMenu(i18n("LSP Client"), this);
        actionCollection()->addAction(QStringLiteral("popup_lspclient"), menu);
        menu->addAction(m_findDef);
        menu->addAction(m_findDecl);
        menu->addAction(m_findRef);
        menu->addAction(m_findImpl);
        menu->addAction(m_triggerHighlight);
        menu->addAction(m_triggerSymbolInfo);
        menu->addAction(m_triggerFormat);
        menu->addAction(m_triggerRename);
        menu->addSeparator();
        menu->addAction(m_diagnosticsSwitch);
        menu->addAction(m_messagesSwitch);
        menu->addAction(m_closeDynamic);
        menu->addSeparator();
        menu->addAction(m_restartServer);
        menu->addAction(m_restartAll);
        menu->addSeparator();
        // more options
        auto moreOptions = new KActionMenu(i18n("More options"), this);
        menu->addAction(moreOptions);
        moreOptions->addAction(m_complDocOn);
        moreOptions->addAction(m_refDeclaration);
        moreOptions->addAction(m_autoHover);
        moreOptions->addAction(m_onTypeFormatting);
        moreOptions->addAction(m_incrementalSync);
        moreOptions->addSeparator();
        moreOptions->addAction(m_diagnostics);
        moreOptions->addAction(m_diagnosticsHighlight);
        moreOptions->addAction(m_diagnosticsMark);
        moreOptions->addAction(m_diagnosticsHover);
        moreOptions->addSeparator();
        moreOptions->addAction(m_messages);
        moreOptions->addAction(m_messagesAutoSwitch);

        // sync with plugin settings if updated
        connect(m_plugin, &LSPClientPlugin::update, this, &self_type::configUpdated);

        // toolview
        m_toolView.reset(mainWin->createToolView(plugin, QStringLiteral("kate_lspclient"), KTextEditor::MainWindow::Bottom, QIcon::fromTheme(QStringLiteral("application-x-ms-dos-executable")), i18n("LSP Client")));
        m_tabWidget = new QTabWidget(m_toolView.data());
        m_toolView->layout()->addWidget(m_tabWidget);
        m_tabWidget->setFocusPolicy(Qt::NoFocus);
        m_tabWidget->setTabsClosable(true);
        KAcceleratorManager::setNoAccel(m_tabWidget);
        connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &self_type::tabCloseRequested);
        connect(m_tabWidget, &QTabWidget::currentChanged, this, &self_type::tabChanged);

        // diagnostics tab
        m_diagnosticsTree = new QTreeView();

        

        // MOD
        m_diagnosticsTree->setAlternatingRowColors(false);




        
        m_diagnosticsTreeOwn.reset(m_diagnosticsTree);
        m_diagnosticsModel.reset(new QStandardItemModel());
        m_diagnosticsModel->setColumnCount(1);
        m_diagnosticsTree->setModel(m_diagnosticsModel.data());
        configureTreeView(m_diagnosticsTree);
        connect(m_diagnosticsTree, &QTreeView::clicked, this, &self_type::goToItemLocation);
        connect(m_diagnosticsTree, &QTreeView::doubleClicked, this, &self_type::triggerCodeAction);

        // messages tab
        m_messagesView = new QPlainTextEdit();
        m_messagesView->setMaximumBlockCount(100);
        m_messagesView->setReadOnly(true);
        m_messagesViewOwn.reset(m_messagesView);

        // track position in view to sync diagnostics list
        m_viewTracker.reset(LSPClientViewTracker::new_(plugin, mainWin, 0, 500));
        connect(m_viewTracker.data(), &LSPClientViewTracker::newState, this, &self_type::onViewState);

        connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this, &LSPClientActionView::onViewCreated);

        connect(this, &LSPClientActionView::ctrlClickDefRecieved, this, &LSPClientActionView::onCtrlMouseMove);

        configUpdated();
        updateState();
    }

    void onViewCreated(KTextEditor::View *view)
    {
        if (view) {
            view->installEventFilter(this);
            auto childs = view->children();
            for (auto c : childs) {
                if (c)
                    c->installEventFilter(this);
            }
        }
    }

    // This is taken from KDevelop :)
    KTextEditor::View* viewFromWidget(QWidget* widget)
    {
        if (!widget)
            return nullptr;
        auto* view = qobject_cast<KTextEditor::View*>(widget);
        if (view)
            return view;
        else
            return viewFromWidget(widget->parentWidget());
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        auto mouseEvent = dynamic_cast<QMouseEvent*>(event);

        // we are only concerned with mouse events for now :)
        if (!mouseEvent)
            return false;

        // common stuff that we need for both events
        auto wid = qobject_cast<QWidget*>(obj);
        auto v = viewFromWidget(wid);
        if (!v)
            return false;

        const auto coords = wid->mapTo(v, mouseEvent->pos());
        const auto cur =  v->coordinatesToCursor(coords);
        // there isn't much we can do now, just bail out
        if (!cur.isValid())
            return false;

        auto doc = v->document();
        if (!doc)
            return false;

        const auto word = doc->wordAt(cur);

        // The user pressed Ctrl + Click
        if (event->type() == QEvent::MouseButtonPress) {
            if (mouseEvent->button() == Qt::LeftButton && mouseEvent->modifiers() == Qt::ControlModifier) {
                // must set cursor else we will be jumping somewhere else!!
                v->setCursorPosition(cur);
                if (!word.isEmpty()) {
                    m_ctrlHoverFeedback.clear(m_mainWindow->activeView());
                    goToDefinition();
                }
            }
        }
        // The user is hovering with Ctrl pressed
        else if (event->type() == QEvent::MouseMove) {
            if (mouseEvent->modifiers() == Qt::ControlModifier) {
                const auto range = doc->wordRangeAt(cur);
                if (!word.isEmpty() && range.isValid()) {
                    m_ctrlHoverFeedback.setRangeAndWidget(range, wid);
                    // this will not go anywhere actually, but just signal whether we have a definition
                    // Also, please rethink very hard if you are going to reuse this method. It's made
                    // only for Ctrl+Hover
                    processCtrlMouseHover(cur);
                } else {
                    // if there is no word, unset the cursor and remove the highlight
                    if (wid)
                        wid->unsetCursor();
                    m_ctrlHoverFeedback.clear(m_mainWindow->activeView());
                }
            } else {
                // simple mouse move, make sure to unset the cursor
                // and remove the highlight
                if (wid)
                    wid->unsetCursor();
                m_ctrlHoverFeedback.clear(m_mainWindow->activeView());
            }
        }

        return false;
    }

    ~LSPClientActionView() override
    {
        // unregister all code-completion providers, else we might crash
        for (auto view : qAsConst(m_completionViews)) {
            qobject_cast<KTextEditor::CodeCompletionInterface *>(view)->unregisterCompletionModel(m_completion.data());
        }

        // unregister all text-hint providers, else we might crash
        for (auto view : qAsConst(m_hoverViews)) {
            qobject_cast<KTextEditor::TextHintInterface *>(view)->unregisterTextHintProvider(m_forwardHover.data());
        }

        clearAllLocationMarks();
        clearAllDiagnosticsMarks();
    }

    void configureTreeView(QTreeView *treeView)
    {
        treeView->setHeaderHidden(true);
        treeView->setFocusPolicy(Qt::NoFocus);
        treeView->setLayoutDirection(Qt::LeftToRight);
        treeView->setSortingEnabled(false);
        treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);

        // context menu
        treeView->setContextMenuPolicy(Qt::CustomContextMenu);
        auto menu = new QMenu(treeView);
        menu->addAction(i18n("Expand All"), treeView, &QTreeView::expandAll);
        menu->addAction(i18n("Collapse All"), treeView, &QTreeView::collapseAll);
        auto h = [menu](const QPoint &) { menu->popup(QCursor::pos()); };
        connect(treeView, &QTreeView::customContextMenuRequested, h);
    }

    void displayOptionChanged()
    {
        m_diagnosticsHighlight->setEnabled(m_diagnostics->isChecked());
        m_diagnosticsMark->setEnabled(m_diagnostics->isChecked());
        m_diagnosticsHover->setEnabled(m_diagnostics->isChecked());
        // messages tab should go first
        int messagesIndex = m_tabWidget->indexOf(m_messagesView);
        if (m_messages->isChecked() && m_messagesViewOwn) {
            m_tabWidget->insertTab(0, m_messagesView, i18nc("@title:tab", "Messages"));
            messagesIndex = 0;
            m_messagesViewOwn.take();
        } else if (!m_messages->isChecked() && !m_messagesViewOwn) {
            m_messagesViewOwn.reset(m_messagesView);
            m_tabWidget->removeTab(messagesIndex);
            messagesIndex = -1;
        }
        // diagnstics tab next
        int diagnosticsIndex = m_tabWidget->indexOf(m_diagnosticsTree);
        // setTabEnabled may still show it ... so let's be more forceful
        if (m_diagnostics->isChecked() && m_diagnosticsTreeOwn) {
            m_diagnosticsTreeOwn.take();
            m_tabWidget->insertTab(messagesIndex + 1, m_diagnosticsTree, i18nc("@title:tab", "Diagnostics"));
        } else if (!m_diagnostics->isChecked() && !m_diagnosticsTreeOwn) {
            m_diagnosticsTreeOwn.reset(m_diagnosticsTree);
            m_tabWidget->removeTab(diagnosticsIndex);
        }
        m_diagnosticsSwitch->setEnabled(m_diagnostics->isChecked());
        m_serverManager->setIncrementalSync(m_incrementalSync->isChecked());
        updateState();
    }

    void configUpdated()
    {
        if (m_complDocOn)
            m_complDocOn->setChecked(m_plugin->m_complDoc);
        if (m_refDeclaration)
            m_refDeclaration->setChecked(m_plugin->m_refDeclaration);
        if (m_autoHover)
            m_autoHover->setChecked(m_plugin->m_autoHover);
        if (m_onTypeFormatting)
            m_onTypeFormatting->setChecked(m_plugin->m_onTypeFormatting);
        if (m_incrementalSync)
            m_incrementalSync->setChecked(m_plugin->m_incrementalSync);
        if (m_diagnostics)
            m_diagnostics->setChecked(m_plugin->m_diagnostics);
        if (m_diagnosticsHighlight)
            m_diagnosticsHighlight->setChecked(m_plugin->m_diagnosticsHighlight);
        if (m_diagnosticsMark)
            m_diagnosticsMark->setChecked(m_plugin->m_diagnosticsMark);
        if (m_diagnosticsHover)
            m_diagnosticsHover->setChecked(m_plugin->m_diagnosticsHover);
        if (m_messages)
            m_messages->setChecked(m_plugin->m_messages);
        if (m_messagesAutoSwitch)
            m_messagesAutoSwitch->setCurrentItem(m_plugin->m_messagesAutoSwitch);
        displayOptionChanged();
    }

    void restartCurrent()
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        auto server = m_serverManager->findServer(activeView);
        if (server)
            m_serverManager->restart(server.data());
    }

    void restartAll()
    {
        m_serverManager->restart(nullptr);
    }

    static void clearMarks(KTextEditor::Document *doc, RangeCollection &ranges, DocumentCollection &docs, uint markType)
    {
        KTextEditor::MarkInterface *iface = docs.contains(doc) ? qobject_cast<KTextEditor::MarkInterface *>(doc) : nullptr;
        if (iface) {
            const QHash<int, KTextEditor::Mark *> marks = iface->marks();
            QHashIterator<int, KTextEditor::Mark *> i(marks);
            while (i.hasNext()) {
                i.next();
                if (i.value()->type & markType) {
                    iface->removeMark(i.value()->line, markType);
                }
            }
            docs.remove(doc);
        }

        for (auto it = ranges.find(doc); it != ranges.end() && it.key() == doc;) {
            delete it.value();
            it = ranges.erase(it);
        }
    }

    static void clearMarks(RangeCollection &ranges, DocumentCollection &docs, uint markType)
    {
        while (!ranges.empty()) {
            clearMarks(ranges.begin().key(), ranges, docs, markType);
        }
    }

    Q_SLOT void clearAllMarks(KTextEditor::Document *doc)
    {
        clearMarks(doc, m_ranges, m_marks, RangeData::markType);
        clearMarks(doc, m_diagnosticsRanges, m_diagnosticsMarks, RangeData::markTypeDiagAll);
    }

    void clearAllLocationMarks()
    {
        clearMarks(m_ranges, m_marks, RangeData::markType);
        // no longer add any again
        m_ownedModel.reset();
        m_markModel.clear();
    }

    void clearAllDiagnosticsMarks()
    {
        clearMarks(m_diagnosticsRanges, m_diagnosticsMarks, RangeData::markTypeDiagAll);
    }

    void addMarks(KTextEditor::Document *doc, QStandardItem *item, RangeCollection *ranges, DocumentCollection *docs)
    {
        Q_ASSERT(item);
        KTextEditor::MovingInterface *miface = qobject_cast<KTextEditor::MovingInterface *>(doc);
        Q_ASSERT(miface);
#if KTEXTEDITOR_VERSION >= QT_VERSION_CHECK(5, 69, 0)
        KTextEditor::MarkInterfaceV2 *iface = qobject_cast<KTextEditor::MarkInterfaceV2 *>(doc);
#else
        KTextEditor::MarkInterface *iface = qobject_cast<KTextEditor::MarkInterface *>(doc);
#endif
        Q_ASSERT(iface);
        KTextEditor::View *activeView = m_mainWindow->activeView();
        KTextEditor::ConfigInterface *ciface = qobject_cast<KTextEditor::ConfigInterface *>(activeView);

        auto url = item->data(RangeData::FileUrlRole).toUrl();
        // document url could end up empty while in intermediate reload state
        // (and then it might match a parent item with no RangeData at all)
        if (url != doc->url() || url.isEmpty())
            return;

        KTextEditor::Range range = item->data(RangeData::RangeRole).value<LSPRange>();
        if (!range.isValid() || range.isEmpty())
            return;
        auto line = range.start().line();
        RangeData::KindEnum kind = RangeData::KindEnum(item->data(RangeData::KindRole).toInt());

        KTextEditor::Attribute::Ptr attr(new KTextEditor::Attribute());

        bool enabled = m_diagnostics && m_diagnostics->isChecked() && m_diagnosticsHighlight && m_diagnosticsHighlight->isChecked();
        KTextEditor::MarkInterface::MarkTypes markType = RangeData::markType;
        switch (kind) {
        case RangeData::KindEnum::Text: {
            // well, it's a bit like searching for something, so re-use that color
            QColor rangeColor = Qt::yellow;
            if (ciface) {
                rangeColor = ciface->configValue(QStringLiteral("search-highlight-color")).value<QColor>();
            }
            attr->setBackground(rangeColor);
            enabled = true;
            break;
        }
        // FIXME are there any symbolic/configurable ways to pick these colors?
        case RangeData::KindEnum::Read:
            attr->setBackground(Qt::green);
            enabled = true;
            break;
        case RangeData::KindEnum::Write:
            attr->setBackground(Qt::red);
            enabled = true;
            break;
        // use underlining for diagnostics to avoid lots of fancy flickering
        case RangeData::KindEnum::Error:
            markType = RangeData::markTypeDiagError;
            attr->setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            attr->setUnderlineColor(Qt::red);
            break;
        case RangeData::KindEnum::Warning:
            markType = RangeData::markTypeDiagWarning;
            attr->setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            attr->setUnderlineColor(QColor(255, 128, 0));
            break;
        case RangeData::KindEnum::Information:
        case RangeData::KindEnum::Hint:
        case RangeData::KindEnum::Related:
            markType = RangeData::markTypeDiagOther;
            attr->setUnderlineStyle(QTextCharFormat::DashUnderline);
            attr->setUnderlineColor(Qt::blue);
            break;
        }
        if (activeView) {
            attr->setForeground(activeView->defaultStyleAttribute(KTextEditor::dsNormal)->foreground().color());
        }

        // highlight the range
        if (enabled && ranges) {
            KTextEditor::MovingRange *mr = miface->newMovingRange(range);
            mr->setAttribute(attr);
            mr->setZDepth(-90000.0); // Set the z-depth to slightly worse than the selection
            mr->setAttributeOnlyForViews(true);
            ranges->insert(doc, mr);
        }

        // add match mark for range
#if KTEXTEDITOR_VERSION < QT_VERSION_CHECK(5, 69, 0)
        const int ps = 32;
#endif
        bool handleClick = true;
        enabled = m_diagnostics && m_diagnostics->isChecked() && m_diagnosticsMark && m_diagnosticsMark->isChecked();
        switch (markType) {
        case RangeData::markType:
            iface->setMarkDescription(markType, i18n("RangeHighLight"));
#if KTEXTEDITOR_VERSION >= QT_VERSION_CHECK(5, 69, 0)
            iface->setMarkIcon(markType, QIcon());
#else
            iface->setMarkPixmap(markType, QIcon().pixmap(0, 0));
#endif
            handleClick = false;
            enabled = true;
            break;
        case RangeData::markTypeDiagError:
            iface->setMarkDescription(markType, i18n("Error"));
#if KTEXTEDITOR_VERSION >= QT_VERSION_CHECK(5, 69, 0)
            iface->setMarkIcon(markType, diagnosticsIcon(LSPDiagnosticSeverity::Error));
#else
            iface->setMarkPixmap(markType, diagnosticsIcon(LSPDiagnosticSeverity::Error).pixmap(ps, ps));
#endif
            break;
        case RangeData::markTypeDiagWarning:
            iface->setMarkDescription(markType, i18n("Warning"));
#if KTEXTEDITOR_VERSION >= QT_VERSION_CHECK(5, 69, 0)
            iface->setMarkIcon(markType, diagnosticsIcon(LSPDiagnosticSeverity::Warning));
#else
            iface->setMarkPixmap(markType, diagnosticsIcon(LSPDiagnosticSeverity::Warning).pixmap(ps, ps));
#endif
            break;
        case RangeData::markTypeDiagOther:
            iface->setMarkDescription(markType, i18n("Information"));
#if KTEXTEDITOR_VERSION >= QT_VERSION_CHECK(5, 69, 0)
            iface->setMarkIcon(markType, diagnosticsIcon(LSPDiagnosticSeverity::Information));
#else
            iface->setMarkPixmap(markType, diagnosticsIcon(LSPDiagnosticSeverity::Information).pixmap(ps, ps));
#endif
            break;
        default:
            Q_ASSERT(false);
            break;
        }
        if (enabled && docs) {
            iface->addMark(line, markType);
            docs->insert(doc);
        }

        // ensure runtime match
        connect(doc, SIGNAL(aboutToInvalidateMovingInterfaceContent(KTextEditor::Document *)), this, SLOT(clearAllMarks(KTextEditor::Document *)), Qt::UniqueConnection);
        connect(doc, SIGNAL(aboutToDeleteMovingInterfaceContent(KTextEditor::Document *)), this, SLOT(clearAllMarks(KTextEditor::Document *)), Qt::UniqueConnection);
        // reload might save/restore marks before/after above signals, so let's clear before that
        connect(doc, &KTextEditor::Document::aboutToReload, this, &self_type::clearAllMarks, Qt::UniqueConnection);

        if (handleClick) {
            connect(doc, SIGNAL(markClicked(KTextEditor::Document *, KTextEditor::Mark, bool &)), this, SLOT(onMarkClicked(KTextEditor::Document *, KTextEditor::Mark, bool &)), Qt::UniqueConnection);
        }
    }

    void addMarksRec(KTextEditor::Document *doc, QStandardItem *item, RangeCollection *ranges, DocumentCollection *docs)
    {
        Q_ASSERT(item);
        addMarks(doc, item, ranges, docs);
        for (int i = 0; i < item->rowCount(); ++i) {
            addMarksRec(doc, item->child(i), ranges, docs);
        }
    }

    void addMarks(KTextEditor::Document *doc, QStandardItemModel *treeModel, RangeCollection &ranges, DocumentCollection &docs)
    {
        // check if already added
        auto oranges = ranges.contains(doc) ? nullptr : &ranges;
        auto odocs = docs.contains(doc) ? nullptr : &docs;

        if (!oranges && !odocs)
            return;

        Q_ASSERT(treeModel);
        addMarksRec(doc, treeModel->invisibleRootItem(), oranges, odocs);
    }

    void goToDocumentLocation(const QUrl &uri, const KTextEditor::Range& location)
    {
        int line = location.start().line();
        int column = location.start().column();
        KTextEditor::View *activeView = m_mainWindow->activeView();
        if (!activeView || uri.isEmpty() || line < 0 || column < 0)
            return;

        KTextEditor::Document *document = activeView->document();
        KTextEditor::Cursor cdef(line, column);

        if (document && uri == document->url()) {
            activeView->setCursorPosition(cdef);
            highlightLandingLocation(activeView, location);
        } else {
            KTextEditor::View *view = m_mainWindow->openUrl(uri);
            if (view) {
                view->setCursorPosition(cdef);
                highlightLandingLocation(view, location);
            }
        }
    }

    /**
     * @brief give a short 1sec temporary highlight where you land
     */
    void highlightLandingLocation(KTextEditor::View* view, const KTextEditor::Range& location)
    {
        auto doc = view->document();
        if (!doc)
            return;
        auto miface = qobject_cast<KTextEditor::MovingInterface*>(doc);
        if (!miface)
            return;
        auto mr = miface->newMovingRange(location);
        KTextEditor::Attribute::Ptr attr(new KTextEditor::Attribute);
        attr->setUnderlineStyle(QTextCharFormat::SingleUnderline);
        mr->setView(view);
        mr->setAttribute(attr);
        QTimer::singleShot(1000, doc, [mr] {
            mr->setRange(KTextEditor::Range::invalid());
            delete mr;
        });
    }

    void goToItemLocation(const QModelIndex &index)
    {
        auto url = index.data(RangeData::FileUrlRole).toUrl();
        auto start = index.data(RangeData::RangeRole).value<LSPRange>();
        goToDocumentLocation(url, start);
    }

    // custom item subclass that captures additional attributes;
    // a bit more convenient than the variant/role way
    struct DiagnosticItem : public QStandardItem {
        LSPDiagnostic m_diagnostic;
        LSPCodeAction m_codeAction;
        QSharedPointer<LSPClientRevisionSnapshot> m_snapshot;

        DiagnosticItem(const LSPDiagnostic &d)
            : m_diagnostic(d)
        {
        }

        DiagnosticItem(const LSPCodeAction &c, QSharedPointer<LSPClientRevisionSnapshot> s)
            : m_codeAction(c)
            , m_snapshot(std::move(s))
        {
            m_diagnostic.range = LSPRange::invalid();
        }

        bool isCodeAction()
        {
            return !m_diagnostic.range.isValid() && m_codeAction.title.size();
        }
    };

    // double click on:
    // diagnostic item -> request and add actions (below item)
    // code action -> perform action (literal edit and/or execute command)
    // (execution of command may lead to an applyEdit request from server)
    void triggerCodeAction(const QModelIndex &index)
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        QPointer<KTextEditor::Document> document = activeView->document();
        auto server = m_serverManager->findServer(activeView);
        auto it = dynamic_cast<DiagnosticItem *>(m_diagnosticsModel->itemFromIndex(index));
        if (!server || !document || !it)
            return;

        // click on an action ?
        if (it->isCodeAction()) {
            auto &action = it->m_codeAction;
            // apply edit before command
            applyWorkspaceEdit(action.edit, it->m_snapshot.data());
            auto &command = action.command;
            if (command.command.size()) {
                // accept edit requests that may be sent to execute command
                m_accept_edit = true;
                // but only for a short time
                QTimer::singleShot(2000, this, [this] { m_accept_edit = false; });
                server->executeCommand(command.command, command.arguments);
            }
            // diagnostics are likely updated soon, but might be clicked again in meantime
            // so clear once executed, so not executed again
            action.edit.changes.clear();
            action.command.command.clear();
            return;
        }

        // only engage action if
        // * active document matches diagnostic document
        // * if really clicked a diagnostic item
        //   (which is the case as it != nullptr and not a code action)
        // * if no code action invoked and added already
        //   (note; related items are also children)
        auto url = it->data(RangeData::FileUrlRole).toUrl();
        if (url != document->url() || it->data(Qt::UserRole).toBool())
            return;

        // store some things to find item safely later on
        QPersistentModelIndex pindex(index);
        QSharedPointer<LSPClientRevisionSnapshot> snapshot(m_serverManager->snapshot(server.data()));
        auto h = [this, url, snapshot, pindex](const QList<LSPCodeAction> &actions) {
            if (!pindex.isValid())
                return;
            auto child = m_diagnosticsModel->itemFromIndex(pindex);
            if (!child)
                return;
            // add actions below diagnostic item
            for (const auto &action : actions) {
                auto item = new DiagnosticItem(action, snapshot);
                child->appendRow(item);
                auto text = action.kind.size() ? QStringLiteral("[%1] %2").arg(action.kind).arg(action.title) : action.title;
                item->setData(text, Qt::DisplayRole);
                item->setData(codeActionIcon(), Qt::DecorationRole);
            }
            m_diagnosticsTree->setExpanded(child->index(), true);
            // mark actions added
            child->setData(true, Qt::UserRole);
        };

        auto range = activeView->selectionRange();
        if (!range.isValid()) {
            range = document->documentRange();
        }
        server->documentCodeAction(url, range, {}, {it->m_diagnostic}, this, h);
    }

    bool tabCloseRequested(int index)
    {
        auto widget = m_tabWidget->widget(index);
        if (widget != m_diagnosticsTree && widget != m_messagesView) {
            if (m_markModel && widget == m_markModel->parent()) {
                clearAllLocationMarks();
            }
            delete widget;
            return true;
        }
        return false;
    }

    void tabChanged(int index)
    {
        // reset to regular foreground
        m_tabWidget->tabBar()->setTabTextColor(index, QColor());
    }

    void switchToDiagnostics()
    {
        m_tabWidget->setCurrentWidget(m_diagnosticsTree);
        m_mainWindow->showToolView(m_toolView.data());
    }

    void switchToMessages()
    {
        m_tabWidget->setCurrentWidget(m_messagesView);
        m_mainWindow->showToolView(m_toolView.data());
    }

    void closeDynamic()
    {
        for (int i = 0; i < m_tabWidget->count();) {
            // if so deemed suitable, tab will be spared and not closed
            if (!tabCloseRequested(i))
                ++i;
        }
    }

    // local helper to overcome some differences in LSP types
    struct RangeItem {
        QUrl uri;
        LSPRange range;
        LSPDocumentHighlightKind kind;

        bool isValid() const
        {
            return uri.isValid() && range.isValid();
        }
    };

    static bool compareRangeItem(const RangeItem &a, const RangeItem &b)
    {
        return (a.uri < b.uri) || ((a.uri == b.uri) && a.range < b.range);
    }

    // provide Qt::DisplayRole (text) line lazily;
    // only find line's text content when so requested
    // This may then involve opening reading some file, at which time
    // all items for that file will be resolved in one go.
    struct LineItem : public QStandardItem {
        KTextEditor::MainWindow *m_mainWindow;

        LineItem(KTextEditor::MainWindow *mainWindow)
            : m_mainWindow(mainWindow)
        {
        }

        QVariant data(int role = Qt::UserRole + 1) const override
        {
            auto rootItem = this->parent();
            if (role != Qt::DisplayRole || !rootItem) {
                return QStandardItem::data(role);
            }

            auto line = data(Qt::UserRole);
            // either of these mean we tried to obtain line already
            if (line.isValid() || rootItem->data(RangeData::KindRole).toBool()) {
                return QStandardItem::data(role).toString().append(line.toString());
            }

            KTextEditor::Document *doc = nullptr;
            QScopedPointer<FileLineReader> fr;
            for (int i = 0; i < rootItem->rowCount(); i++) {
                auto child = rootItem->child(i);
                if (i == 0) {
                    auto url = child->data(RangeData::FileUrlRole).toUrl();
                    doc = findDocument(m_mainWindow, url);
                    if (!doc) {
                        fr.reset(new FileLineReader(url));
                    }
                }
                auto lineno = child->data(RangeData::RangeRole).value<LSPRange>().start().line();
                auto line = doc ? doc->line(lineno) : fr->line(lineno);
                child->setData(line, Qt::UserRole);
            }

            // mark as processed
            rootItem->setData(RangeData::KindRole, true);

            // should work ok
            return data(role);
        }
    };

    LSPRange transformRange(const QUrl &url, const LSPClientRevisionSnapshot &snapshot, const LSPRange &range)
    {
        KTextEditor::MovingInterface *miface;
        qint64 revision;

        auto result = range;
        snapshot.find(url, miface, revision);
        if (miface) {
            miface->transformRange(result, KTextEditor::MovingRange::DoNotExpand, KTextEditor::MovingRange::AllowEmpty, revision);
        }
        return result;
    }

    void fillItemRoles(QStandardItem *item, const QUrl &url, const LSPRange _range, RangeData::KindEnum kind, const LSPClientRevisionSnapshot *snapshot = nullptr)
    {
        auto range = snapshot ? transformRange(url, *snapshot, _range) : _range;
        item->setData(QVariant(url), RangeData::FileUrlRole);
        QVariant vrange;
        vrange.setValue(range);
        item->setData(vrange, RangeData::RangeRole);
        item->setData(static_cast<int>(kind), RangeData::KindRole);
    }

    void makeTree(const QVector<RangeItem> &locations, const LSPClientRevisionSnapshot *snapshot)
    {
        // group by url, assuming input is suitably sorted that way
        auto treeModel = new QStandardItemModel();
        treeModel->setColumnCount(1);

        QUrl lastUrl;
        QStandardItem *parent = nullptr;
        for (const auto &loc : locations) {
            // ensure we create a parent, if not already there (bug 427270) or we have a different url
            if (!parent || loc.uri != lastUrl) {
                if (parent) {
                    parent->setText(QStringLiteral("%1: %2").arg(lastUrl.toLocalFile()).arg(parent->rowCount()));
                }
                lastUrl = loc.uri;
                parent = new QStandardItem();
                treeModel->appendRow(parent);
            }
            auto item = new LineItem(m_mainWindow);
            parent->appendRow(item);
            // add partial display data; line will be added by item later on
            item->setText(i18n("Line: %1: ", loc.range.start().line() + 1));
            fillItemRoles(item, loc.uri, loc.range, loc.kind, snapshot);
        }
        if (parent)
            parent->setText(QStringLiteral("%1: %2").arg(lastUrl.toLocalFile()).arg(parent->rowCount()));

        // plain heuristic; mark for auto-expand all when safe and/or useful to do so
        if (treeModel->rowCount() <= 2 || locations.size() <= 20) {
            treeModel->invisibleRootItem()->setData(true, RangeData::KindRole);
        }

        m_ownedModel.reset(treeModel);
        m_markModel = treeModel;
    }

    void showTree(const QString &title, QPointer<QTreeView> *targetTree)
    {
        // clean up previous target if any
        if (targetTree && *targetTree) {
            int index = m_tabWidget->indexOf(*targetTree);
            if (index >= 0)
                tabCloseRequested(index);
        }

        // setup view
        auto treeView = new QTreeView();
        configureTreeView(treeView);

        // transfer model from owned to tree and that in turn to tabwidget
        auto treeModel = m_ownedModel.take();
        treeView->setModel(treeModel);
        treeModel->setParent(treeView);
        int index = m_tabWidget->addTab(treeView, title);
        connect(treeView, &QTreeView::clicked, this, &self_type::goToItemLocation);

        if (treeModel->invisibleRootItem()->data(RangeData::KindRole).toBool()) {
            treeView->expandAll();
        }

        // track for later cleanup
        if (targetTree)
            *targetTree = treeView;

        // activate the resulting tab
        m_tabWidget->setCurrentIndex(index);
        m_mainWindow->showToolView(m_toolView.data());
    }

    void showMessage(const QString &text, KTextEditor::Message::MessageType level)
    {
        KTextEditor::View *view = m_mainWindow->activeView();
        if (!view || !view->document())
            return;

        auto kmsg = new KTextEditor::Message(text, level);
        kmsg->setPosition(KTextEditor::Message::BottomInView);
        kmsg->setAutoHide(500);
        kmsg->setView(view);
        view->document()->postMessage(kmsg);
    }

    void handleEsc(QEvent *e)
    {
        if (!m_mainWindow)
            return;

        QKeyEvent *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Escape && k->modifiers() == Qt::NoModifier) {
            if (!m_ranges.empty()) {
                clearAllLocationMarks();
            } else if (m_toolView->isVisible()) {
                m_mainWindow->hideToolView(m_toolView.data());
            }
        }
    }

    template<typename Handler> using LocationRequest = std::function<LSPClientServer::RequestHandle(LSPClientServer &, const QUrl &document, const LSPPosition &pos, const QObject *context, const Handler &h)>;

    template<typename Handler> void positionRequest(const LocationRequest<Handler> &req, const Handler &h, QScopedPointer<LSPClientRevisionSnapshot> *snapshot = nullptr,  KTextEditor::Cursor cur = KTextEditor::Cursor::invalid())
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        auto server = m_serverManager->findServer(activeView);
        if (!server)
            return;

        // track revision if requested
        if (snapshot) {
            snapshot->reset(m_serverManager->snapshot(server.data()));
        }

        KTextEditor::Cursor cursor = cur.isValid() ? cur : activeView->cursorPosition();

        clearAllLocationMarks();
        m_req_timeout = false;
        QTimer::singleShot(1000, this, [this] { m_req_timeout = true; });
        m_handle.cancel() = req(*server, activeView->document()->url(), {cursor.line(), cursor.column()}, this, h);
    }

    QString currentWord()
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        if (activeView) {
            KTextEditor::Cursor cursor = activeView->cursorPosition();
            return activeView->document()->wordAt(cursor);
        } else {
            return QString();
        }
    }

    Q_SIGNAL void ctrlClickDefRecieved(const RangeItem& range);

    Q_SLOT void onCtrlMouseMove(const RangeItem& range)
    {
        if (range.isValid()) {
            if (m_ctrlHoverFeedback.isValid())
                m_ctrlHoverFeedback.highlight(m_mainWindow->activeView());
        }
    }

    // some template and function type trickery here, but at least that buck stops here then ...
    template<typename ReplyEntryType, bool doshow = true, typename HandlerType = ReplyHandler<QList<ReplyEntryType>>>
    void processLocations(const QString &title,
                          const typename utils::identity<LocationRequest<HandlerType>>::type &req,
                          bool onlyshow,
                          const std::function<RangeItem(const ReplyEntryType &)> &itemConverter,
                          QPointer<QTreeView> *targetTree = nullptr)
    {
        // no capture for move only using initializers available (yet), so shared outer type
        // the additional level of indirection is so it can be 'filled-in' after lambda creation
        QSharedPointer<QScopedPointer<LSPClientRevisionSnapshot>> s(new QScopedPointer<LSPClientRevisionSnapshot>);
        auto h = [this, title, onlyshow, itemConverter, targetTree, s](const QList<ReplyEntryType> &defs) {
            if (defs.count() == 0) {
                showMessage(i18n("No results"), KTextEditor::Message::Information);
            } else {
                // convert to helper type
                QVector<RangeItem> ranges;
                ranges.reserve(defs.size());
                for (const auto &def : defs) {
                    ranges.push_back(itemConverter(def));
                }
                // ... so we can sort it also
                std::stable_sort(ranges.begin(), ranges.end(), compareRangeItem);
                makeTree(ranges, s.data()->data());

                // assuming that reply ranges refer to revision when submitted
                // (not specified anyway in protocol/reply)
                if (defs.count() > 1 || onlyshow) {
                    showTree(title, targetTree);
                }
                // it's not nice to jump to some location if we are too late
                if (!m_req_timeout && !onlyshow) {
                    // assuming here that the first location is the best one
                    const auto &item = itemConverter(defs.at(0));
                    goToDocumentLocation(item.uri, item.range);
                    // forego mark and such if only a single destination
                    if (defs.count() == 1) {
                        clearAllLocationMarks();
                    }
                }
                // update marks
                updateState();
            }
        };

        positionRequest<HandlerType>(req, h, s.data());
    }

    /**
     * @brief processCtrlMouseHover This function just processes Ctrl + Mouse move hovering.
     * It should not be used for other purposes ideally.
     */
    void processCtrlMouseHover(const KTextEditor::Cursor& cursor)
    {
        auto h = [this](const QList<LSPLocation> &defs) {
            if (defs.isEmpty()) {
                return;
            } else {
                const auto item = locationToRangeItem(defs.at(0));
                emit this->ctrlClickDefRecieved(item);
                return;
            }
        };

        using Handler = std::function<void(const QList<LSPLocation> &)>;
        auto request = &LSPClientServer::documentDefinition;
        positionRequest<Handler>(request, h, nullptr, cursor);
    }


    static RangeItem locationToRangeItem(const LSPLocation &loc)
    {
        return {loc.uri, loc.range, LSPDocumentHighlightKind::Text};
    }

    void goToDefinition()
    {
        auto title = i18nc("@title:tab", "Definition: %1", currentWord());
        processLocations<LSPLocation>(title, &LSPClientServer::documentDefinition, false, &self_type::locationToRangeItem, &m_defTree);
    }

    void goToDeclaration()
    {
        auto title = i18nc("@title:tab", "Declaration: %1", currentWord());
        processLocations<LSPLocation>(title, &LSPClientServer::documentDeclaration, false, &self_type::locationToRangeItem, &m_declTree);
    }

    void findReferences()
    {
        auto title = i18nc("@title:tab", "References: %1", currentWord());
        bool decl = m_refDeclaration->isChecked();
        // clang-format off
        auto req = [decl](LSPClientServer &server, const QUrl &document, const LSPPosition &pos, const QObject *context, const DocumentDefinitionReplyHandler &h)
        { return server.documentReferences(document, pos, decl, context, h); };
        // clang-format on

        processLocations<LSPLocation>(title, req, true, &self_type::locationToRangeItem);
    }

    void findImplementation()
    {
        auto title = i18nc("@title:tab", "Implementation: %1", currentWord());
        // clang-format off
        auto req = [](LSPClientServer &server, const QUrl &document, const LSPPosition &pos, const QObject *context, const DocumentDefinitionReplyHandler &h)
        { return server.documentImplementation(document, pos, context, h); };
        // clang-format on

        processLocations<LSPLocation>(title, req, true, &self_type::locationToRangeItem);
    }

    void highlight()
    {
        // determine current url to capture and use later on
        QUrl url;
        const KTextEditor::View *viewForRequest(m_mainWindow->activeView());
        if (viewForRequest && viewForRequest->document()) {
            url = viewForRequest->document()->url();
        }

        auto title = i18nc("@title:tab", "Highlight: %1", currentWord());
        auto converter = [url](const LSPDocumentHighlight &hl) { return RangeItem {url, hl.range, hl.kind}; };

        processLocations<LSPDocumentHighlight, false>(title, &LSPClientServer::documentHighlight, true, converter);
    }

    void symbolInfo()
    {
        // trigger manually the normally automagic hover
        if (auto activeView = m_mainWindow->activeView()) {
            m_hover->textHint(activeView, activeView->cursorPosition());
        }
    }

    void applyEdits(KTextEditor::Document *doc, const LSPClientRevisionSnapshot *snapshot, const QList<LSPTextEdit> &edits)
    {
        KTextEditor::MovingInterface *miface = qobject_cast<KTextEditor::MovingInterface *>(doc);
        Q_ASSERT(miface);

        // NOTE:
        // server might be pretty sloppy wrt edits (e.g. python-language-server)
        // e.g. send one edit for the whole document rather than 'surgical edits'
        // and that even when requesting format for a limited selection
        // ... but then we are but a client and do as we are told
        // all-in-all a low priority feature

        // all coordinates in edits are wrt original document,
        // so create moving ranges that will adjust to preceding edits as they are applied
        QVector<KTextEditor::MovingRange *> ranges;
        for (const auto &edit : edits) {
            auto range = snapshot ? transformRange(doc->url(), *snapshot, edit.range) : edit.range;
            KTextEditor::MovingRange *mr = miface->newMovingRange(range);
            ranges.append(mr);
        }

        // now make one transaction (a.o. for one undo) and apply in sequence
        {
            KTextEditor::Document::EditingTransaction transaction(doc);
            for (int i = 0; i < ranges.length(); ++i) {
                doc->replaceText(ranges.at(i)->toRange(), edits.at(i).newText);
            }
        }

        qDeleteAll(ranges);
    }

    void applyWorkspaceEdit(const LSPWorkspaceEdit &edit, const LSPClientRevisionSnapshot *snapshot)
    {
        auto currentView = m_mainWindow->activeView();
        for (auto it = edit.changes.begin(); it != edit.changes.end(); ++it) {
            auto document = findDocument(m_mainWindow, it.key());
            if (!document) {
                KTextEditor::View *view = m_mainWindow->openUrl(it.key());
                if (view) {
                    document = view->document();
                }
            }
            applyEdits(document, snapshot, it.value());
        }
        if (currentView) {
            m_mainWindow->activateView(currentView->document());
        }
    }

    void onApplyEdit(const LSPApplyWorkspaceEditParams &edit, const ApplyEditReplyHandler &h, bool &handled)
    {
        if (handled)
            return;
        handled = true;

        if (m_accept_edit) {
            qCInfo(LSPCLIENT) << "applying edit" << edit.label;
            applyWorkspaceEdit(edit.edit, nullptr);
        } else {
            qCInfo(LSPCLIENT) << "ignoring edit";
        }
        h({m_accept_edit, QString()});
    }

    template<typename Collection> void checkEditResult(const Collection &c)
    {
        if (c.empty()) {
            showMessage(i18n("No edits"), KTextEditor::Message::Information);
        }
    }

    void delayCancelRequest(LSPClientServer::RequestHandle &&h, int timeout_ms = 4000)
    {
        QTimer::singleShot(timeout_ms, this, [h]() mutable { h.cancel(); });
    }

    void format(QChar lastChar = QChar())
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        QPointer<KTextEditor::Document> document = activeView->document();
        auto server = m_serverManager->findServer(activeView);
        if (!server || !document)
            return;

        int tabSize = 4;
        bool insertSpaces = true;
        auto cfgiface = qobject_cast<KTextEditor::ConfigInterface *>(document);
        Q_ASSERT(cfgiface);
        tabSize = cfgiface->configValue(QStringLiteral("tab-width")).toInt();
        insertSpaces = cfgiface->configValue(QStringLiteral("replace-tabs")).toBool();

        // sigh, no move initialization capture ...
        // (again) assuming reply ranges wrt revisions submitted at this time
        QSharedPointer<LSPClientRevisionSnapshot> snapshot(m_serverManager->snapshot(server.data()));
        auto h = [this, document, snapshot, lastChar](const QList<LSPTextEdit> &edits) {
            if (lastChar.isNull()) {
                checkEditResult(edits);
            }
            if (document) {
                applyEdits(document, snapshot.data(), edits);
            }
        };

        auto options = LSPFormattingOptions {tabSize, insertSpaces, QJsonObject()};
        auto handle = !lastChar.isNull() ? server->documentOnTypeFormatting(document->url(), activeView->cursorPosition(), lastChar, options, this, h)
                                         : (activeView->selection() ? server->documentRangeFormatting(document->url(), activeView->selectionRange(), options, this, h) : server->documentFormatting(document->url(), options, this, h));
        delayCancelRequest(std::move(handle));
    }

    void rename()
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        QPointer<KTextEditor::Document> document = activeView->document();
        auto server = m_serverManager->findServer(activeView);
        if (!server || !document)
            return;

        bool ok = false;
        // results are typically (too) limited
        // due to server implementation or limited view/scope
        // so let's add a disclaimer that it's not our fault
        QString newName = QInputDialog::getText(activeView, i18nc("@title:window", "Rename"), i18nc("@label:textbox", "New name (caution: not all references may be replaced)"), QLineEdit::Normal, QString(), &ok);
        if (!ok) {
            return;
        }

        QSharedPointer<LSPClientRevisionSnapshot> snapshot(m_serverManager->snapshot(server.data()));
        auto h = [this, snapshot](const LSPWorkspaceEdit &edit) {
            checkEditResult(edit.changes);
            applyWorkspaceEdit(edit, snapshot.data());
        };
        auto handle = server->documentRename(document->url(), activeView->cursorPosition(), newName, this, h);
        delayCancelRequest(std::move(handle));
    }

    static QStandardItem *getItem(const QStandardItemModel &model, const QUrl &url)
    {
        auto l = model.findItems(url.toLocalFile());
        if (l.length()) {
            return l.at(0);
        }
        return nullptr;
    }

    static QStandardItem *getItem(const QStandardItem *topItem, KTextEditor::Cursor pos, bool onlyLine)
    {
        QStandardItem *targetItem = nullptr;
        if (topItem) {
            int count = topItem->rowCount();
            // let's not run wild on a linear search in a flood of diagnostics
            // user is already in enough trouble as it is ;-)
            if (count > 50)
                count = 0;
            for (int i = 0; i < count; ++i) {
                auto item = topItem->child(i);
                auto range = item->data(RangeData::RangeRole).value<LSPRange>();
                if ((onlyLine && pos.line() == range.start().line()) || (range.contains(pos))) {
                    targetItem = item;
                    break;
                }
            }
        }
        return targetItem;
    }

    // select/scroll to diagnostics item for document and (optionally) line
    bool syncDiagnostics(KTextEditor::Document *document, int line, bool allowTop, bool doShow)
    {
        if (!m_diagnosticsTree)
            return false;

        auto hint = QAbstractItemView::PositionAtTop;
        QStandardItem *topItem = getItem(*m_diagnosticsModel, document->url());
        QStandardItem *targetItem = getItem(topItem, {line, 0}, true);
        if (targetItem) {
            hint = QAbstractItemView::PositionAtCenter;
        }
        if (!targetItem && allowTop) {
            targetItem = topItem;
        }
        if (targetItem) {
            m_diagnosticsTree->blockSignals(true);
            m_diagnosticsTree->scrollTo(targetItem->index(), hint);
            m_diagnosticsTree->setCurrentIndex(targetItem->index());
            m_diagnosticsTree->blockSignals(false);
            if (doShow) {
                m_tabWidget->setCurrentWidget(m_diagnosticsTree);
                m_mainWindow->showToolView(m_toolView.data());
            }
        }
        return targetItem != nullptr;
    }

    void onViewState(KTextEditor::View *view, LSPClientViewTracker::State newState)
    {
        if (!view || !view->document())
            return;

        // select top item on view change,
        // but otherwise leave selection unchanged if no match
        switch (newState) {
        case LSPClientViewTracker::ViewChanged:
            syncDiagnostics(view->document(), view->cursorPosition().line(), true, false);
            break;
        case LSPClientViewTracker::LineChanged:
            syncDiagnostics(view->document(), view->cursorPosition().line(), false, false);
            break;
        default:
            // should not happen
            break;
        }
    }

    Q_SLOT void onMarkClicked(KTextEditor::Document *document, KTextEditor::Mark mark, bool &handled)
    {
        // no action if no mark was sprinkled here
        if (m_diagnosticsMarks.contains(document) && syncDiagnostics(document, mark.line, false, true)) {
            handled = true;
        }
    }

    void onDiagnostics(const LSPPublishDiagnosticsParams &diagnostics)
    {
        if (!m_diagnosticsTree)
            return;

        QStandardItemModel *model = m_diagnosticsModel.data();
        QStandardItem *topItem = getItem(*m_diagnosticsModel, diagnostics.uri);

        if (!topItem) {
            // no need to create an empty one
            if (diagnostics.diagnostics.empty()) {
                return;
            }
            topItem = new QStandardItem();
            model->appendRow(topItem);
            topItem->setText(diagnostics.uri.toLocalFile());
        } else {
            topItem->setRowCount(0);
        }

        for (const auto &diag : diagnostics.diagnostics) {
            auto item = new DiagnosticItem(diag);
            topItem->appendRow(item);
            QString source;
            if (diag.source.length()) {
                source = QStringLiteral("[%1] ").arg(diag.source);
            }
            item->setData(diagnosticsIcon(diag.severity), Qt::DecorationRole);
            item->setText(source + diag.message);
            fillItemRoles(item, diagnostics.uri, diag.range, diag.severity);
            const auto &relatedInfo = diag.relatedInformation;
            for (const auto &related : relatedInfo) {
                if (related.location.uri.isEmpty()) {
                    continue;
                }
                auto relatedItemMessage = new QStandardItem();
                fillItemRoles(relatedItemMessage, related.location.uri, related.location.range, RangeData::KindEnum::Related);
                auto basename = QFileInfo(related.location.uri.toLocalFile()).fileName();
                auto location = QStringLiteral("%1:%2").arg(basename).arg(related.location.range.start().line());
                relatedItemMessage->setText(QStringLiteral("[%1] %2").arg(location).arg(related.message));
                relatedItemMessage->setData(diagnosticsIcon(LSPDiagnosticSeverity::Information), Qt::DecorationRole);
                item->appendRow({relatedItemMessage});
                m_diagnosticsTree->setExpanded(item->index(), true);
            }
        }

        // TODO perhaps add some custom delegate that only shows 1 line
        // and only the whole text when item selected ??
        m_diagnosticsTree->setExpanded(topItem->index(), true);
        m_diagnosticsTree->setRowHidden(topItem->row(), QModelIndex(), topItem->rowCount() == 0);
        m_diagnosticsTree->scrollTo(topItem->index(), QAbstractItemView::PositionAtTop);

        updateState();
        // also sync updated diagnositic to current position
        auto currentView = m_mainWindow->activeView();
        if (currentView && currentView->document())
            syncDiagnostics(currentView->document(), currentView->cursorPosition().line(), false, false);
    }

    QString onTextHint(KTextEditor::View *view, const KTextEditor::Cursor &position)
    {
        QString result;
        auto document = view->document();

        if (!m_diagnosticsTree || !m_diagnosticsModel || !document)
            return result;

        bool autoHover = m_autoHover && m_autoHover->isChecked();
        bool diagHover = m_diagnostics && m_diagnostics->isChecked() && m_diagnosticsHover && m_diagnosticsHover->isChecked();

        QStandardItem *topItem = diagHover ? getItem(*m_diagnosticsModel, document->url()) : nullptr;
        QStandardItem *targetItem = getItem(topItem, position, false);
        if (targetItem) {
            result = targetItem->text();
            // also include related info
            int count = targetItem->rowCount();
            for (int i = 0; i < count; ++i) {
                auto item = targetItem->child(i);
                result += QStringLiteral("\n<br>");
                result += item->text();
            }
            // but let's not get carried away too far
            const int maxsize = m_plugin->m_diagnosticsSize;
            if (result.size() > maxsize) {
                result.resize(maxsize);
                result.append(QStringLiteral("..."));
            }
        } else if (autoHover) {
            // only trigger generic hover if no diagnostic to show;
            // avoids interference by generic hover info
            // (which is likely not so useful in this case/position anyway)
            result = m_hover->textHint(view, position);
        }

        return result;
    }

    KTextEditor::View *viewForUrl(const QUrl &url) const
    {
        for (auto *view : m_mainWindow->views()) {
            if (view->document()->url() == url)
                return view;
        }
        return nullptr;
    }

    void addMessage(LSPMessageType level, const QString &header, const QString &msg)
    {
        if (!m_messagesView)
            return;

        QString lvl = i18nc("@info", "Unknown");
        switch (level) {
        case LSPMessageType::Error:
            lvl = i18nc("@info", "Error");
            break;
        case LSPMessageType::Warning:
            lvl = i18nc("@info", "Warning");
            break;
        case LSPMessageType::Info:
            lvl = i18nc("@info", "Information");
            break;
        case LSPMessageType::Log:
            lvl = i18nc("@info", "Log");
            break;
        }

        // let's consider this expert info and use ISO date
        auto now = QDateTime::currentDateTime().toString(Qt::ISODate);
        auto text = QStringLiteral("[%1] [%2] [%3]\n%4\n").arg(now).arg(lvl).arg(header).arg(msg.trimmed());
        m_messagesView->appendPlainText(text);

        if (static_cast<int>(level) <= m_messagesAutoSwitch->currentItem()) {
            switchToMessages();
        } else {
            // show arrival of new message
            auto index = m_tabWidget->indexOf(m_messagesView);
            if (m_tabWidget->currentIndex() != index)
                m_tabWidget->tabBar()->setTabTextColor(index, Qt::gray);
        }
    }

    // params type is same for show or log and is treated the same way
    void onMessage(const LSPLogMessageParams &params)
    {
        // determine server description
        auto server = dynamic_cast<LSPClientServer *>(sender());
        auto desc = i18nc("@info", "LSP Server");
        if (server)
            desc += QStringLiteral(": %1").arg(LSPClientServerManager::serverDescription(server));
        addMessage(params.type, desc, params.message);
    }

    void onShowMessage(KTextEditor::Message::MessageType level, const QString &msg)
    {
        // translate level
        LSPMessageType lvl = LSPMessageType::Log;
        using KMessage = KTextEditor::Message;
        switch (level) {
        case KMessage::Error:
            lvl = LSPMessageType::Error;
            break;
        case KMessage::Warning:
            lvl = LSPMessageType::Warning;
            break;
        case KMessage::Information:
            lvl = LSPMessageType::Info;
            break;
        case KMessage::Positive:
            lvl = LSPMessageType::Log;
            break;
        }

        addMessage(lvl, i18nc("@info", "LSP Client"), msg);
    }

    Q_SLOT void clearSemanticHighlighting(KTextEditor::Document *document)
    {
        auto &documentRanges = m_semanticHighlightRanges[document];
        for (const auto &lineRanges : documentRanges)
            qDeleteAll(lineRanges);
        documentRanges.clear();
    }

    void onSemanticHighlighting(const LSPSemanticHighlightingParams &params)
    {
        auto *view = viewForUrl(params.textDocument.uri);
        if (!view) {
            qCWarning(LSPCLIENT) << "failed to find view for uri" << params.textDocument.uri;
            return;
        }

        auto server = m_serverManager->findServer(view);
        if (!server) {
            qCWarning(LSPCLIENT) << "failed to find server for view" << params.textDocument.uri;
            return;
        }

        auto *document = view->document();
        auto *miface = qobject_cast<KTextEditor::MovingInterface *>(document);
        Q_ASSERT(miface);

        // TODO: translate between locked revision, if possible?
        auto version = params.textDocument.version;
        if (version == -1) { // use version from disk
            version = miface->lastSavedRevision();
            if (version == -1) { // never saved
                version = miface->revision();
            }
        }
        if (version != miface->revision()) {
            qCWarning(LSPCLIENT) << "discarding highlighting, versions don't match:" << params.textDocument.version << version << miface->revision();
            return;
        }

        // ensure runtime match
        connect(document, SIGNAL(aboutToInvalidateMovingInterfaceContent(KTextEditor::Document *)), this, SLOT(clearSemanticHighlighting(KTextEditor::Document *)), Qt::UniqueConnection);
        connect(document, SIGNAL(aboutToDeleteMovingInterfaceContent(KTextEditor::Document *)), this, SLOT(clearSemanticHighlighting(KTextEditor::Document *)), Qt::UniqueConnection);

        // TODO: make schema attributes accessible via some new interface,
        // or at least add configuration to the lsp plugin config
        // FIXME: static attributes break if one e.g. switches the color scheme on the fly!
        auto attributeForScopes = [view](const QVector<QString> &scopes) -> KTextEditor::Attribute::Ptr {
            for (const auto &scope : scopes) {
                if (scope == QLatin1String("entity.name.function.method.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsFunction);
                        attr.detach();
                        attr->setForeground(Qt::darkYellow);
                        attr->setFontItalic(true);
                    }
                    return attr;
                } else if (scope == QLatin1String("entity.name.function.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsFunction);
                        attr.detach();
                        attr->setForeground(Qt::darkYellow);
                    }
                    return attr;
                } else if (scope == QLatin1String("variable.other.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsVariable);
                        attr.detach();
                        attr->setForeground(Qt::darkCyan);
                    }
                    return attr;
                } else if (scope == QLatin1String("variable.other.field.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsVariable);
                        attr.detach();
                        attr->setForeground(Qt::darkCyan);
                        attr->setFontItalic(true);
                    }
                    return attr;
                } else if (scope == QLatin1String("entity.name.type.enum.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsConstant);
                        attr.detach();
                        attr->setForeground(Qt::darkMagenta);
                    }
                    return attr;
                } else if (scope == QLatin1String("variable.other.enummember.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsConstant);
                        attr.detach();
                        attr->setForeground(Qt::darkMagenta);
                        attr->setFontItalic(true);
                    }
                    return attr;
                } else if (scope == QLatin1String("entity.name.type.class.cpp") || scope == QLatin1String("entity.name.type.template.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsDataType);
                        attr.detach();
                        attr->setForeground(Qt::darkMagenta);
                    }
                    return attr;
                } else if (scope == QLatin1String("entity.name.namespace.cpp")) {
                    static KTextEditor::Attribute::Ptr attr;
                    if (!attr) {
                        attr = view->defaultStyleAttribute(KTextEditor::dsDataType);
                        attr.detach();
                        attr->setForeground(Qt::darkGreen);
                        attr->setFontItalic(true);
                    }
                    return attr;
                }
            }
            return {};
        };

        // TODO: we should try to recycle the moving ranges instead of recreating them all the time

        const auto scopes = server->capabilities().semanticHighlightingProvider.scopes;
        // qDebug() << params.textDocument.uri << scopes;

        auto &documentRanges = m_semanticHighlightRanges[document];
        QSet<int> handledLines;
        for (const auto &line : params.lines) {
            handledLines.insert(line.line);
            auto &lineRanges = documentRanges[line.line];
            qDeleteAll(lineRanges);
            lineRanges.clear();
            // qDebug() << "line:" << line.line;
            for (const auto &token : line.tokens) {
                // qDebug() << "token:" << token.character << token.length << token.scope << scopes.value(token.scope);
                auto attribute = attributeForScopes(scopes.value(token.scope));
                if (!attribute)
                    continue;

                const auto columnStart = static_cast<int>(token.character);
                const auto columnEnd = columnStart + static_cast<int>(token.length);
                constexpr auto expand = KTextEditor::MovingRange::ExpandLeft | KTextEditor::MovingRange::ExpandRight;
                auto *range = miface->newMovingRange({line.line, columnStart, line.line, columnEnd}, expand, KTextEditor::MovingRange::InvalidateIfEmpty);
                range->setAttribute(attribute);
            }
        }
        // clear lines that got removed or commented out
        for (auto it = documentRanges.begin(); it != documentRanges.end();) {
            if (!handledLines.contains(it.key())) {
                qDeleteAll(it.value());
                it = documentRanges.erase(it);
            } else {
                ++it;
            }
        }
    }

    void onDocumentUrlChanged(KTextEditor::Document *doc)
    {
        // url already changed by this time and new url not useful
        (void)doc;
        // note; url also changes when closed
        // spec says;
        // if a language has a project system, diagnostics are not cleared by *server*
        // but in either case (url change or close); remove lingering diagnostics
        // collect active urls
        QSet<QString> fpaths;
        for (const auto &view : m_mainWindow->views()) {
            if (auto doc = view->document()) {
                fpaths.insert(doc->url().toLocalFile());
            }
        }
        // check and clear defunct entries
        const auto &model = *m_diagnosticsModel;
        for (int i = 0; i < model.rowCount(); ++i) {
            auto item = model.item(i);
            if (item && !fpaths.contains(item->text())) {
                item->setRowCount(0);
                if (m_diagnosticsTree) {
                    m_diagnosticsTree->setRowHidden(item->row(), QModelIndex(), true);
                }
            }
        }
    }

    void onTextChanged(KTextEditor::Document *doc)
    {
        if (m_onTypeFormattingTriggers.empty())
            return;

        KTextEditor::View *activeView = m_mainWindow->activeView();
        if (!activeView || activeView->document() != doc)
            return;

        // NOTE the intendation mode should probably be set to None,
        // so as not to experience unpleasant interference
        auto cursor = activeView->cursorPosition();
        QChar lastChar = cursor.column() == 0 ? QChar::fromLatin1('\n') : doc->characterAt({cursor.line(), cursor.column() - 1});
        if (m_onTypeFormattingTriggers.contains(lastChar)) {
            format(lastChar);
        }
    }

    void updateState()
    {
        KTextEditor::View *activeView = m_mainWindow->activeView();
        auto doc = activeView ? activeView->document() : nullptr;
        auto server = m_serverManager->findServer(activeView);
        bool defEnabled = false, declEnabled = false, refEnabled = false, implEnabled = false;
        bool hoverEnabled = false, highlightEnabled = false;
        bool formatEnabled = false;
        bool renameEnabled = false;

        if (server) {
            const auto &caps = server->capabilities();
            defEnabled = caps.definitionProvider;
            // FIXME no real official protocol way to detect, so enable anyway
            declEnabled = caps.declarationProvider || true;
            refEnabled = caps.referencesProvider;
            implEnabled = caps.implementationProvider;
            hoverEnabled = caps.hoverProvider;
            highlightEnabled = caps.documentHighlightProvider;
            formatEnabled = caps.documentFormattingProvider || caps.documentRangeFormattingProvider;
            renameEnabled = caps.renameProvider;

            connect(server.data(), &LSPClientServer::publishDiagnostics, this, &self_type::onDiagnostics, Qt::UniqueConnection);
            connect(server.data(), &LSPClientServer::showMessage, this, &self_type::onMessage, Qt::UniqueConnection);
            connect(server.data(), &LSPClientServer::logMessage, this, &self_type::onMessage, Qt::UniqueConnection);
            connect(server.data(), &LSPClientServer::semanticHighlighting, this, &self_type::onSemanticHighlighting, Qt::UniqueConnection);
            connect(server.data(), &LSPClientServer::applyEdit, this, &self_type::onApplyEdit, Qt::UniqueConnection);

            // update format trigger characters
            const auto &fmt = caps.documentOnTypeFormattingProvider;
            if (fmt.provider && m_onTypeFormatting->isChecked()) {
                m_onTypeFormattingTriggers = fmt.triggerCharacters;
            } else {
                m_onTypeFormattingTriggers.clear();
            }
            // and monitor for such
            if (doc) {
                connect(doc, &KTextEditor::Document::textChanged, this, &self_type::onTextChanged, Qt::UniqueConnection);
                connect(doc, &KTextEditor::Document::documentUrlChanged, this, &self_type::onDocumentUrlChanged, Qt::UniqueConnection);
            }
        }

        if (m_findDef)
            m_findDef->setEnabled(defEnabled);
        if (m_findDecl)
            m_findDecl->setEnabled(declEnabled);
        if (m_findRef)
            m_findRef->setEnabled(refEnabled);
        if (m_findImpl)
            m_findImpl->setEnabled(implEnabled);
        if (m_triggerHighlight)
            m_triggerHighlight->setEnabled(highlightEnabled);
        if (m_triggerSymbolInfo)
            m_triggerSymbolInfo->setEnabled(hoverEnabled);
        if (m_triggerFormat)
            m_triggerFormat->setEnabled(formatEnabled);
        if (m_triggerRename)
            m_triggerRename->setEnabled(renameEnabled);
        if (m_complDocOn)
            m_complDocOn->setEnabled(server);
        if (m_restartServer)
            m_restartServer->setEnabled(server);

        // update completion with relevant server
        m_completion->setServer(server);
        if (m_complDocOn)
            m_completion->setSelectedDocumentation(m_complDocOn->isChecked());
        updateCompletion(activeView, server.data());

        // update hover with relevant server
        m_hover->setServer(server && server->capabilities().hoverProvider ? server : nullptr);
        // need hover either for generic documentHover or for diagnostics
        // so register anyway if server available and will sort out what to do/show later
        updateHover(activeView, server.data());

        // update marks if applicable
        if (m_markModel && doc)
            addMarks(doc, m_markModel, m_ranges, m_marks);
        if (m_diagnosticsModel && doc) {
            clearMarks(doc, m_diagnosticsRanges, m_diagnosticsMarks, RangeData::markTypeDiagAll);
            addMarks(doc, m_diagnosticsModel.data(), m_diagnosticsRanges, m_diagnosticsMarks);
        }

        // connect for cleanup stuff
        if (activeView)
            connect(activeView, &KTextEditor::View::destroyed, this, &self_type::viewDestroyed, Qt::UniqueConnection);
    }

    void viewDestroyed(QObject *view)
    {
        m_completionViews.remove(static_cast<KTextEditor::View *>(view));
        m_hoverViews.remove(static_cast<KTextEditor::View *>(view));
    }

    void updateCompletion(KTextEditor::View *view, LSPClientServer *server)
    {
        if (!view) {
            return;
        }

        bool registered = m_completionViews.contains(view);

        KTextEditor::CodeCompletionInterface *cci = qobject_cast<KTextEditor::CodeCompletionInterface *>(view);
        Q_ASSERT(cci);

        if (!registered && server && server->capabilities().completionProvider.provider) {
            qCInfo(LSPCLIENT) << "registering cci";
            cci->registerCompletionModel(m_completion.data());
            m_completionViews.insert(view);
        }

        if (registered && !server) {
            qCInfo(LSPCLIENT) << "unregistering cci";
            cci->unregisterCompletionModel(m_completion.data());
            m_completionViews.remove(view);
        }
    }

    void updateHover(KTextEditor::View *view, LSPClientServer *server)
    {
        if (!view) {
            return;
        }

        bool registered = m_hoverViews.contains(view);

        KTextEditor::TextHintInterface *cci = qobject_cast<KTextEditor::TextHintInterface *>(view);
        Q_ASSERT(cci);

        if (!registered && server) {
            qCInfo(LSPCLIENT) << "registering thi";
            cci->registerTextHintProvider(m_forwardHover.data());
            m_hoverViews.insert(view);
        }

        if (registered && !server) {
            qCInfo(LSPCLIENT) << "unregistering thi";
            cci->unregisterTextHintProvider(m_forwardHover.data());
            m_hoverViews.remove(view);
        }
    }
};

class LSPClientPluginViewImpl : public QObject, public KXMLGUIClient
{
    Q_OBJECT

    typedef LSPClientPluginViewImpl self_type;

    KTextEditor::MainWindow *m_mainWindow;
    QSharedPointer<LSPClientServerManager> m_serverManager;
    QScopedPointer<LSPClientActionView> m_actionView;

public:
    LSPClientPluginViewImpl(LSPClientPlugin *plugin, KTextEditor::MainWindow *mainWin)
        : QObject(mainWin)
        , m_mainWindow(mainWin)
        , m_serverManager(LSPClientServerManager::new_(plugin, mainWin))
    {
        KXMLGUIClient::setComponentName(QStringLiteral("lspclient"), i18n("LSP Client"));
        setXMLFile(QStringLiteral("ui.rc"));

        // we need to do this AFTER the setComponentName above
        m_actionView.reset(new LSPClientActionView(plugin, mainWin, this, m_serverManager));

        m_mainWindow->guiFactory()->addClient(this);
    }

    ~LSPClientPluginViewImpl() override
    {
        // minimize/avoid some surprises;
        // safe construction/destruction by separate (helper) objects;
        // signals are auto-disconnected when high-level "view" objects are broken down
        // so it only remains to clean up lowest level here then prior to removal
        m_actionView.reset();
        m_serverManager.reset();
        m_mainWindow->guiFactory()->removeClient(this);
    }
};

QObject *LSPClientPluginView::new_(LSPClientPlugin *plugin, KTextEditor::MainWindow *mainWin)
{
    return new LSPClientPluginViewImpl(plugin, mainWin);
}

#include "lspclientpluginview.moc"

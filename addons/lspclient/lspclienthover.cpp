/*  SPDX-License-Identifier: MIT

    SPDX-FileCopyrightText: 2019 Mark Nauwelaerts <mark.nauwelaerts@gmail.com>
    SPDX-FileCopyrightText: 2019 Christoph Cullmann <cullmann@kde.org>

    SPDX-License-Identifier: MIT
*/

#include "lspclienthover.h"
#include "lspclientplugin.h"

#include "lspclient_debug.h"

#include <KTextEditor/Cursor>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QToolTip>
#include <utility>

class LSPClientHoverImpl : public LSPClientHover
{
    Q_OBJECT

    typedef LSPClientHoverImpl self_type;

    QSharedPointer<LSPClientServerManager> m_manager;
    QSharedPointer<LSPClientServer> m_server;

    LSPClientServer::RequestHandle m_handle;

public:
    LSPClientHoverImpl(QSharedPointer<LSPClientServerManager> manager)
        : m_manager(std::move(manager))
        , m_server(nullptr)
    {
    }

    void setServer(QSharedPointer<LSPClientServer> server) override
    {
        m_server = server;
    }

    /**
     * This function is called whenever the users hovers over text such
     * that the text hint delay passes. Then, textHint() is called
     * for each registered TextHintProvider.
     *
     * Return the text hint (possibly Qt richtext) for @p view at @p position.
     *
     * If you do not have any contents to show, just return an empty QString().
     *
     * \param view the view that requests the text hint
     * \param position text cursor under the mouse position
     * \return text tool tip to be displayed, may be Qt richtext
     */
    QString textHint(KTextEditor::View *view, const KTextEditor::Cursor &position) override
    {
        // hack: delayed handling of tooltip on our own, the API is too dumb for a-sync feedback ;=)
        if (m_server) {
            QPointer<KTextEditor::View> v(view);
            auto h = [this, v, position](const LSPHover &info) {
                if (!v || info.contents.isEmpty()) {
                    return;
                }

                // combine contents elements to one string
                QString finalTooltip;
                for (auto &element : info.contents) {
                    if (!finalTooltip.isEmpty()) {
                        finalTooltip.append(QLatin1Char('\n'));
                    }
                    finalTooltip.append(element.value);
                }

                // we need to cut this a bit if too long until we have
                // something more sophisticated than a tool tip for it
                if (finalTooltip.size() > 512) {
                    finalTooltip.resize(512);
                    finalTooltip.append(QStringLiteral("..."));
                }

                // show tool tip: think about a better way for "large" stuff
                QToolTip::showText(v->mapToGlobal(v->cursorToCoordinate(position)), finalTooltip);
            };

            m_handle.cancel() = m_server->documentHover(view->document()->url(), position, this, h);
        }

        return QString();
    }
};

LSPClientHover *LSPClientHover::new_(QSharedPointer<LSPClientServerManager> manager)
{
    return new LSPClientHoverImpl(std::move(manager));
}

#include "lspclienthover.moc"

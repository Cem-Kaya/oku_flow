#ifdef _WIN32
#include "okuflow/ui/assistant_chat_view.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/theme_contrast.hpp"
#include <QApplication>
#include <QBuffer>
#include <QImageReader>
#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>

namespace okuflow {
namespace {
// Keep Qt's checkable accessibility and keyboard behavior, with a compact
// camera/switch presentation instead of a second long composer label.
class ChatAttachmentSwitch final : public QCheckBox {
public:
    explicit ChatAttachmentSwitch(QWidget* parent=nullptr):QCheckBox(parent)
    {
        setText(QStringLiteral("Attach current view"));
        setMinimumSize(80,44);
        setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override {return QSize(80,44);}
    QSize minimumSizeHint() const override {return sizeHint();}
protected:
    bool hitButton(const QPoint& point) const override {return rect().contains(point);}
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const auto group=isEnabled()?QPalette::Active:QPalette::Disabled;
        const auto ink=palette().color(group,QPalette::ButtonText);
        const auto visual=[this](const QRect& area) {return QStyle::visualRect(layoutDirection(),rect(),area);};
        icon().paint(&painter,visual(QRect(6,12,20,20)),Qt::AlignCenter,isEnabled()?QIcon::Normal:QIcon::Disabled);
        const QRect track=visual(QRect(32,10,44,24));
        const QColor fill=palette().color(group,isChecked()?QPalette::Highlight:QPalette::Button);
        QColor thumb=palette().color(group,isChecked()?QPalette::HighlightedText:QPalette::ButtonText);
        if(theme_contrast::ContrastRatio(fill,thumb)<3.0)
            thumb=theme_contrast::ContrastRatio(fill,Qt::white)>theme_contrast::ContrastRatio(fill,Qt::black)?Qt::white:Qt::black;
        painter.setPen(QPen(ink,1.5));painter.setBrush(fill);
        painter.drawRoundedRect(track,12,12);
        const QRect knob=visual(QRect(isChecked()?55:35,13,18,18));
        painter.setPen(Qt::NoPen);painter.setBrush(thumb);painter.drawEllipse(knob);
        if(hasFocus()) {
            painter.setPen(QPen(ink,2));painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(1,1,-1,-1),5,5);
        }
    }
};
QPushButton* Action(const QString& text,const char* name,QWidget* parent=nullptr)
{
    auto* button=new QPushButton(text,parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setAutoDefault(false); button->setDefault(false);
    button->setMinimumHeight(44);
    button->setAccessibleName(text);
    return button;
}
QString Escape(const QString& text) { return text.toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral("<br>")); }
class ChatBlockIdentity final : public QTextBlockUserData {
public:
    QString messageId;
    int part{}; // heading or message body, both root-level paragraphs
};
struct ChatTextAnchor {
    QString messageId;
    int part{}, offset{}, absolute{};
};
ChatTextAnchor CaptureChatAnchor(QTextDocument* document,int position)
{
    const auto block=document->findBlock(position);
    const auto* identity=dynamic_cast<ChatBlockIdentity*>(block.userData());
    return {identity?identity->messageId:QString(),identity?identity->part:0,
            position-block.position(),position};
}
std::optional<int> RestoreChatAnchor(QTextDocument* document,const ChatTextAnchor& anchor)
{
    if(anchor.messageId.isEmpty())return {};
    for(auto block=document->begin();block.isValid();block=block.next()) {
        const auto* identity=dynamic_cast<ChatBlockIdentity*>(block.userData());
        if(identity && identity->messageId==anchor.messageId && identity->part==anchor.part)
            return block.position()+std::clamp(anchor.offset,0,std::max(0,block.length()-1));
    }
    return {};
}
void TagChatBlocks(QTextDocument* document,const QJsonArray& messages)
{
    auto block=document->begin();int index=0;
    for(const auto value:messages) {
        const auto id=value.toObject().value("id").toString();
        const QString identity=id.isEmpty()?QStringLiteral("index/%1").arg(index):QStringLiteral("id/")+id;
        for(int part=0;part<2 && block.isValid();++part,block=block.next()) {
            auto* data=new ChatBlockIdentity;
            data->messageId=identity;data->part=part;block.setUserData(data);
        }
        ++index;
    }
}
}
AssistantChatView::AssistantChatView(QWidget* parent):QWidget(parent)
{
    setObjectName(QStringLiteral("assistantChatView"));setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral(
        "QWidget#assistantChatView { background: palette(base); }"
        "QPushButton:focus, QToolButton:focus, QPlainTextEdit:focus, QCheckBox:focus { border: 3px solid palette(highlight); }"
        "QFrame#chatDrawer { background: palette(base); border: 1px solid palette(mid); }"
        "QTextBrowser#chatMessages { background: palette(base); border: none; padding: 12px; }"
        "QPlainTextEdit#chatComposer { background: palette(base); border: 1px solid palette(mid); border-radius: 6px; padding: 8px; }"
        "QPushButton#chatSend { background: palette(highlight); color: palette(highlighted-text); border-radius: 6px; padding: 6px 16px; min-width: 84px; }"));
    auto* root=new QVBoxLayout(this); root->setContentsMargins(0,0,0,0); root->setSpacing(0);
    auto* header=new QWidget;
    header_=header;header->setObjectName(QStringLiteral("chatHeader"));
    auto* bar=new QHBoxLayout(header); bar->setContentsMargins(12,4,12,4); bar->setSpacing(6);
    header->setMinimumHeight(52);
    connectionDot_=new QLabel;connectionDot_->setFixedSize(12,12);connectionDot_->setAccessibleName(QStringLiteral("Assistant Connection"));
    bar->addWidget(connectionDot_);SetConnectionState(false);
    connection_=new QLabel(QStringLiteral("Starting Codex..."));
    connection_->setTextFormat(Qt::PlainText);
    connection_->setMinimumWidth(0);
    connection_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    bar->addWidget(connection_,1);
    connect_=Action(QStringLiteral("Connect ChatGPT"),"chatConnect");
    new_=Action(QStringLiteral("New"),"chatNew");
    new_->setIcon(theme_contrast::ThemedIcon(QStringLiteral(":/okuflow/icons/chat-new.svg"),palette().color(QPalette::ButtonText),palette().color(QPalette::Disabled,QPalette::ButtonText)));
    new_->setAccessibleName(QStringLiteral("New Conversation"));
    new_->setIconSize(QSize(24,24));bar->addWidget(new_);
    auto* historyButton=new QToolButton;
    historyButton->setObjectName(QStringLiteral("chatHistory"));
    historyButton->setText(QStringLiteral("History"));
    historyButton->setIcon(theme_contrast::ThemedIcon(QStringLiteral(":/okuflow/icons/chat-history.svg"),palette().color(QPalette::ButtonText),palette().color(QPalette::Disabled,QPalette::ButtonText)));
    historyButton->setIconSize(QSize(24,24));historyButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    historyButton->setAccessibleName(QStringLiteral("Assistant History"));
    historyButton->setToolTip(QStringLiteral("Assistant History"));
    historyButton->setMinimumSize(44,44); bar->addWidget(historyButton);
    auto* settingsButton=new QToolButton;
    settingsButton->setObjectName(QStringLiteral("chatSettings"));
    settingsButton->setText(QStringLiteral("Settings"));
    settingsButton->setIcon(theme_contrast::ThemedIcon(QStringLiteral(":/okuflow/icons/chat-settings.svg"),palette().color(QPalette::ButtonText),palette().color(QPalette::Disabled,QPalette::ButtonText)));
    settingsButton->setIconSize(QSize(24,24));settingsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    settingsButton->setAccessibleName(QStringLiteral("Assistant settings"));
    settingsButton->setToolTip(QStringLiteral("Assistant settings"));
    settingsButton->setMinimumSize(44,44); bar->addWidget(settingsButton);
    root->addWidget(header);
    body_=new QWidget;
    auto* bodyLayout=new QVBoxLayout(body_); bodyLayout->setContentsMargins(0,0,0,0);
    bodyContent_=new QWidget;
    auto* messages=new QVBoxLayout(bodyContent_); messages->setContentsMargins(0,0,0,0); messages->setSpacing(6);
    transcript_=new QTextBrowser;
    transcript_->setObjectName(QStringLiteral("chatMessages"));
    transcript_->setOpenLinks(false); transcript_->setOpenExternalLinks(false);
    transcript_->setAccessibleName(QStringLiteral("Assistant Conversation"));
    transcript_->viewport()->installEventFilter(this);
    QFont font=transcript_->font();font.setPixelSize(20);transcript_->setFont(font);
    messages->addWidget(transcript_,1);bodyLayout->addWidget(bodyContent_);
    root->addWidget(body_,1);

    const auto drawer=[this](const QString& title)->QWidget* {
        auto* frame=new QFrame(body_); frame->setObjectName(QStringLiteral("chatDrawer"));
        auto* layout=new QVBoxLayout(frame); layout->setContentsMargins(12,12,12,12);
        auto* row=new QHBoxLayout;
        auto* label=new QLabel(title);label->setTextFormat(Qt::PlainText);row->addWidget(label,1);
        auto* close=Action(QStringLiteral("Close"),"chatCloseDrawer");row->addWidget(close);
        connect(close,&QPushButton::clicked,this,&AssistantChatView::CloseDrawer);
        layout->addLayout(row);
        auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),frame);
        escape->setContext(Qt::WidgetWithChildrenShortcut);
        connect(escape,&QShortcut::activated,this,&AssistantChatView::CloseDrawer);
        frame->hide();return frame;
    };
    settingsDrawer_=drawer(QStringLiteral("Assistant settings"));
    auto* settings=qobject_cast<QVBoxLayout*>(settingsDrawer_->layout());
    auto* ai=Action(QStringLiteral("AI Settings"),"chatAISettings");
    connect(ai,&QPushButton::clicked,this,&AssistantChatView::AISettingsRequested);
    settings->addWidget(ai);
    settings->addWidget(connect_);
    usage_=new QLabel;usage_->setTextFormat(Qt::PlainText);usage_->setWordWrap(true);settings->addWidget(usage_);
    settingsScroll_=new QScrollArea;settingsScroll_->setWidgetResizable(true);settingsScroll_->setFrameShape(QFrame::NoFrame);settings->addWidget(settingsScroll_,1);
    historyDrawer_=drawer(QStringLiteral("Assistant History"));
    auto* historyLayout=qobject_cast<QVBoxLayout*>(historyDrawer_->layout());
    history_=new QListWidget;history_->setAccessibleName(QStringLiteral("Assistant History"));history_->setWordWrap(true);
    historyLayout->addWidget(history_,1);
    auto* historyActions=new QHBoxLayout;
    rename_=Action(QStringLiteral("Rename"),"chatRename");export_=Action(QStringLiteral("Export"),"chatExport");delete_=Action(QStringLiteral("Delete"),"chatDelete");
    for (auto* button:{rename_,export_,delete_})historyActions->addWidget(button);
    historyLayout->addLayout(historyActions);
    connect(history_,&QListWidget::itemActivated,this,[this](QListWidgetItem* row) {emit ChatSelected(row->data(Qt::UserRole).toString());CloseDrawer();});
    connect(new_,&QPushButton::clicked,this,&AssistantChatView::ChatNewRequested);
    connect(historyButton,&QToolButton::clicked,this,[this]{ShowHistory();emit ChatHistoryRequested();});
    connect(settingsButton,&QToolButton::clicked,this,&AssistantChatView::ShowSettings);

    actionStrip_=new QWidget;
    auto* active=new QHBoxLayout(actionStrip_);active->setContentsMargins(12,2,12,2);
    stop_=Action(QStringLiteral("Stop"),"chatStop");stopUndo_=Action(QStringLiteral("Stop and undo (Ctrl+Shift+U)"),"chatStopUndo");
    readAloud_=Action(QStringLiteral("Read Aloud"),"chatReadAloud");
    active->addWidget(stop_);active->addWidget(stopUndo_);active->addStretch(1);
    root->addWidget(actionStrip_);actionStrip_->hide();
    connect(stop_,&QPushButton::clicked,this,&AssistantChatView::StopRequested);
    connect(stopUndo_,&QPushButton::clicked,this,&AssistantChatView::StopAndUndoRequested);
    connect(readAloud_,&QPushButton::clicked,this,[this] {
        auto rows=snapshot_.value("messages").toArray();
        for (auto it=rows.constEnd();it!=rows.constBegin();) {
            const auto row=(--it)->toObject();
            if(row.value("role").toString()=="assistant" && row.value("status").toString()=="completed" && row.value("complete").toBool()) {
                emit ChatReadAloudRequested(row.value("id").toString(),row.value("text").toString());return;
            }
        }
    });
    auto* composerArea=new QWidget;
    auto* compose=new QVBoxLayout(composerArea);compose->setContentsMargins(12,8,12,12);compose->setSpacing(6);
    composer_=new QPlainTextEdit;composer_->setObjectName(QStringLiteral("chatComposer"));
    composer_->setPlaceholderText(QStringLiteral("Ask about this view..."));
    composer_->setAccessibleName(QStringLiteral("Assistant Question"));
    composer_->installEventFilter(this);composer_->setTabChangesFocus(true);
    composer_->setMinimumHeight(64);composer_->setMaximumHeight(82);compose->addWidget(composer_);
    auto* composeActions=new QHBoxLayout;
    composeActions->setSpacing(6);
    composeActions->addWidget(readAloud_);composeActions->addStretch(1);
    attachment_=new ChatAttachmentSwitch;attachment_->setObjectName(QStringLiteral("chatAttachView"));
    attachment_->setAccessibleName(QStringLiteral("Allow camera views for this turn"));
    attachment_->setToolTip(QStringLiteral("Camera views are sent to the selected provider. This permission ends with the turn or a camera source change."));
    attachment_->setChecked(false);composeActions->addWidget(attachment_);
    send_=Action(QStringLiteral("Send"),"chatSend");send_->setIcon(theme_contrast::ThemedIcon(QStringLiteral(":/okuflow/icons/chat-send.svg"),palette().color(QPalette::HighlightedText),palette().color(QPalette::Disabled,QPalette::ButtonText)));send_->setIconSize(QSize(22,22));
    composeActions->addWidget(send_);compose->addLayout(composeActions);root->addWidget(composerArea);
    connect(composer_,&QPlainTextEdit::textChanged,this,[this]{RefreshActions();emit ChatDraftEdited(composer_->toPlainText());});
    connect(attachment_,&QCheckBox::toggled,this,&AssistantChatView::ChatAttachmentChanged);
    connect(send_,&QPushButton::clicked,this,[this]{if(!snapshot_.value("busy").toBool()&&!composer_->toPlainText().trimmed().isEmpty())emit ChatSubmitted(composer_->toPlainText());});
    const QList<QWidget*> focusOrder{new_,historyButton,settingsButton,transcript_,stop_,stopUndo_,composer_,readAloud_,attachment_,send_};
    for(int i=1;i<focusOrder.size();++i)QWidget::setTabOrder(focusOrder[i-1],focusOrder[i]);
    QWidget::setTabOrder(history_,rename_);QWidget::setTabOrder(rename_,export_);QWidget::setTabOrder(export_,delete_);
    RefreshIcons();RefreshActions();
}
void AssistantChatView::SetChatSnapshot(const QJsonObject& value)
{
    const bool same=snapshot_.value("conversationId")==value.value("conversationId");
    snapshot_=value;
    {const QSignalBlocker block(composer_);if(composer_->toPlainText()!=value.value("draft").toString())composer_->setPlainText(value.value("draft").toString());}
    SetAttachCurrentView(value.value("attachCurrentView").toBool());
    if(!same){transcript_->clear();renderedHtml_.clear();}
    RenderMessages();RefreshActions();
}
void AssistantChatView::RenderMessages()
{
    const auto cursor=transcript_->textCursor();const int oldScroll=transcript_->verticalScrollBar()->value();
    const bool hadSelection=cursor.hasSelection();
    const auto oldAnchor=CaptureChatAnchor(transcript_->document(),cursor.anchor());
    const auto oldPosition=CaptureChatAnchor(transcript_->document(),cursor.position());
    const auto topCursor=transcript_->cursorForPosition(QPoint(0,0));
    const auto topAnchor=CaptureChatAnchor(transcript_->document(),topCursor.position());
    const int topOffset=transcript_->cursorRect(topCursor).top();
    const bool follow=!hadSelection && oldScroll>=transcript_->verticalScrollBar()->maximum()-4;
    const bool dark=palette().color(QPalette::Base).lightness()<128;
    const QString textColor=palette().color(QPalette::Text).name();
    const QString userColor=dark?QStringLiteral("#1d3d5a"):QStringLiteral("#dcecff");
    const QString assistantColor=dark?QStringLiteral("#303030"):QStringLiteral("#f0f1f3");
    const bool rtl=layoutDirection()==Qt::RightToLeft;
    QString html=QStringLiteral("<html><head><style>p { margin:0px; } table { margin:0px; }</style></head><body dir='%1' style='font-size:20px;color:%2'>").arg(rtl?QStringLiteral("rtl"):QStringLiteral("ltr"),textColor);
    QList<QPair<QUrl,QImage>> previews;
    int messageIndex=0;
    for(const auto value:snapshot_.value("messages").toArray()) {
        const auto message=value.toObject();const bool user=message.value("role").toString()=="user";
        const QString role=TranslateUi(user?QStringLiteral("You"):QStringLiteral("Assistant"));
        const qint64 time=message.value("updatedAt").toInteger();
        const QString timestamp=time>0?locale().toString(QDateTime::fromMSecsSinceEpoch(time).time(),QLocale::ShortFormat):QString{};
        QString body=Escape(message.value("text").toString().left(262144));
        const QString status=message.value("status").toString();
        if(body.isEmpty()&&(status=="pending"||status=="streaming"))body=QStringLiteral("…");
        if(status=="failed")body+=QStringLiteral("<br><span style='font-size:14px'>%1</span>")
            .arg(Escape(TranslateUi(QStringLiteral("The request could not finish."))));
        if(status=="interrupted")body+=QStringLiteral("<br><span style='font-size:14px'>%1</span>")
            .arg(Escape(TranslateUi(QStringLiteral("Request stopped"))));
        for(const auto receipt:message.value("actions").toArray()) {
            const QString text=receipt.toString().left(2048);
            if(!text.isEmpty())body+=QStringLiteral("<br><span style='font-size:16px'><b>%1</b></span>").arg(Escape(text));
        }
        const QString origin=message.value("origin").toString()==QStringLiteral("explain")
            ?QStringLiteral(" <img src='okuflow-chat-origin' width='16' height='16'> <span style='font-size:13px'>%1</span>").arg(Escape(TranslateUi(QStringLiteral("Explain")))):QString{};
        const QString heading=QStringLiteral("<b>%1</b> <span style='font-size:13px'>%2</span><br>").arg(Escape(role),Escape(timestamp)).replace(QStringLiteral("<br>"),QString{})+origin;
        QFont bodyFont=transcript_->font();bodyFont.setPixelSize(20);const QFontMetrics metrics(bodyFont);
        int measured=std::max(160,metrics.horizontalAdvance(role+QStringLiteral(" ")+timestamp+QStringLiteral(" ")+TranslateUi(QStringLiteral("Explain")))+24);
        for(const auto& line:message.value("text").toString().split(QLatin1Char('\n')))measured=std::max(measured,metrics.horizontalAdvance(line)+24);
        for(const auto& receipt:message.value("actions").toArray())measured=std::max(measured,metrics.horizontalAdvance(receipt.toString())+24);
        const int available=std::max(1,transcript_->viewport()->width()-24);
        const int maximum=std::max(1,available*85/100);
        int bubble=std::clamp(measured,std::min(160,maximum),maximum);
        QString preview;
        const auto encoded=message.value("imagePreviewJpeg").toString().toLatin1();
        if(user && !encoded.isEmpty() && encoded.size()<=88*1024) {
            QByteArray bytes=QByteArray::fromBase64(encoded,QByteArray::AbortOnBase64DecodingErrors);
            QBuffer buffer(&bytes);buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer,"JPEG");
            const auto size=reader.size();
            if(bytes.size()<=64*1024 && size.isValid() && size.width()<=320 && size.height()<=320) {
                const auto image=reader.read();
                if(!image.isNull()) {
                    bubble=std::min(maximum,std::max(bubble,image.width()));
                    const auto shown=image.size().scaled(QSize(std::min({280,bubble,image.width()}),320),Qt::KeepAspectRatio);
                    const QUrl url(QStringLiteral("okuflow-chat-preview-%1").arg(messageIndex));
                    previews.append({url,image});
                    preview=QStringLiteral("<img src='%1' width='%2' height='%3' alt='%4'><br>")
                        .arg(url.toString()).arg(shown.width()).arg(shown.height()).arg(Escape(TranslateUi(QStringLiteral("Image"))));
                    // A changed image must refresh even if its dimensions match.
                    preview+=QStringLiteral("<!--%1-->").arg(qHash(bytes));
                }
            }
        }
        const int leading=(user!=rtl)?available-bubble:0,trailing=(user!=rtl)?0:available-bubble;
        // Root-level paragraphs keep QTextLayout positions in document coordinates.
        // QTextEdit accessibility omits containing table/frame offsets.
        const QString margins=QStringLiteral("margin-left:%1px;margin-right:%2px;").arg(leading).arg(trailing);
        html+=QStringLiteral("<p style='%1margin-top:6px;margin-bottom:4px'>%2</p><p style='%1background-color:%3;margin-top:0px;margin-bottom:16px'>%4%5</p>")
            .arg(margins,heading,user?userColor:assistantColor,preview,body);
        ++messageIndex;

    }
    html+=QStringLiteral("</body></html>");
    if(renderedHtml_==html)return;
    const auto originIcon=theme_contrast::ThemedIcon(QStringLiteral(":/okuflow/icons/camera.svg"),palette().color(QPalette::Text),palette().color(QPalette::Disabled,QPalette::Text)).pixmap(16,16);
    transcript_->document()->addResource(QTextDocument::ImageResource,QUrl(QStringLiteral("okuflow-chat-origin")),originIcon);
    for(const auto& preview:previews)transcript_->document()->addResource(QTextDocument::ImageResource,preview.first,preview.second);
    renderedHtml_=html;transcript_->setHtml(html);
    TagChatBlocks(transcript_->document(),snapshot_.value("messages").toArray());
    for(const auto& preview:previews)transcript_->document()->addResource(QTextDocument::ImageResource,preview.first,preview.second);
    transcript_->document()->addResource(QTextDocument::ImageResource,QUrl(QStringLiteral("okuflow-chat-origin")),originIcon);
    {
        QTextCursor restored(transcript_->document());
        const int end=std::max(0,transcript_->document()->characterCount()-1);
        const auto anchor=RestoreChatAnchor(transcript_->document(),oldAnchor);
        const auto position=RestoreChatAnchor(transcript_->document(),oldPosition);
        if((!oldAnchor.messageId.isEmpty() && !anchor) || (!oldPosition.messageId.isEmpty() && !position)) {
            // A removed message must not transfer selection to unrelated text.
            restored.setPosition(position.value_or(anchor.value_or(end)));
        } else {
            restored.setPosition(anchor.value_or(std::min(oldAnchor.absolute,end)));
            restored.setPosition(position.value_or(std::min(oldPosition.absolute,end)),QTextCursor::KeepAnchor);
        }
        transcript_->setTextCursor(restored);
    }
    if(follow)transcript_->verticalScrollBar()->setValue(transcript_->verticalScrollBar()->maximum());
    else if(const auto position=RestoreChatAnchor(transcript_->document(),topAnchor)) {
        QTextCursor top(transcript_->document());top.setPosition(*position);
        const int delta=transcript_->cursorRect(top).top()-topOffset;
        transcript_->verticalScrollBar()->setValue(transcript_->verticalScrollBar()->value()+delta);
    } else transcript_->verticalScrollBar()->setValue(oldScroll);
}
void AssistantChatView::RefreshActions()
{
    if(!send_||!composer_)return;
    const bool busy=snapshot_.value("busy").toBool();send_->setEnabled(!busy&&!composer_->toPlainText().trimmed().isEmpty());
    attachment_->setEnabled(!busy||attachment_->isChecked());
    stop_->setVisible(busy);stop_->setEnabled(busy);stopUndo_->setVisible(busy||undoAvailable_);
    bool completed=false;for(const auto row:snapshot_.value("messages").toArray()){const auto message=row.toObject();completed|=message.value("role").toString()=="assistant"&&message.value("status").toString()=="completed"&&message.value("complete").toBool();}
    readAloud_->setEnabled(completed&&!busy);actionStrip_->setVisible(busy||undoAvailable_);
}
void AssistantChatView::SetSettingsWidget(QWidget* widget){settingsScroll_->setWidget(widget);}
void AssistantChatView::RefreshTabOrder()
{
    QWidget* previous=transcript_;
    for (auto* row : {approval_,routing_}) {
        if (!row) continue;
        for (auto* target : row->findChildren<QWidget*>()) {
            if (target->focusPolicy() & Qt::TabFocus) {
                QWidget::setTabOrder(previous,target);previous=target;
            }
        }
    }
    for (QWidget* target : {static_cast<QWidget*>(stop_),static_cast<QWidget*>(stopUndo_),
            static_cast<QWidget*>(composer_),static_cast<QWidget*>(readAloud_),
            static_cast<QWidget*>(attachment_),static_cast<QWidget*>(send_)}) {
        QWidget::setTabOrder(previous,target);previous=target;
    }
}
void AssistantChatView::SetApprovalWidget(QWidget* widget){approval_=widget;qobject_cast<QVBoxLayout*>(bodyContent_->layout())->addWidget(widget);RefreshTabOrder();}
void AssistantChatView::SetRoutingWidget(QWidget* widget){routing_=widget;qobject_cast<QVBoxLayout*>(bodyContent_->layout())->addWidget(widget);RefreshTabOrder();}
void AssistantChatView::SetAttachCurrentView(bool value){const QSignalBlocker block(attachment_);attachment_->setChecked(value);}
bool AssistantChatView::attachCurrentView() const{return attachment_->isChecked();}
void AssistantChatView::SetUndoAvailable(bool value){undoAvailable_=value;RefreshActions();}
void AssistantChatView::SetConnectionState(bool connected)
{
    connectionDot_->setStyleSheet(connected?QStringLiteral("background:#138b47;border-radius:6px;"):QStringLiteral("background:#777777;border-radius:6px;"));
}
void AssistantChatView::OpenDrawer(QWidget* drawer)
{
    restoreFocus_=QApplication::focusWidget();settingsDrawer_->hide();historyDrawer_->hide();
    drawer->setGeometry(body_->rect());drawer->show();drawer->raise();
    if(auto* close=drawer->findChild<QPushButton*>(QStringLiteral("chatCloseDrawer")))close->setFocus(Qt::OtherFocusReason);
}
void AssistantChatView::ShowSettings(){OpenDrawer(settingsDrawer_);}
void AssistantChatView::ShowHistory(){OpenDrawer(historyDrawer_);}
bool AssistantChatView::DrawerContainsFocus() const
{
    auto* focus = QApplication::focusWidget();
    for (auto* drawer : {settingsDrawer_, historyDrawer_})
        if (drawer->isVisible() && focus && (focus == drawer || drawer->isAncestorOf(focus))) return true;
    return false;
}
void AssistantChatView::CloseDrawer(){settingsDrawer_->hide();historyDrawer_->hide();if(restoreFocus_&&restoreFocus_->isVisible())restoreFocus_->setFocus(Qt::OtherFocusReason);}
void AssistantChatView::FocusApproval(QWidget* deny){CloseDrawer();if(deny)deny->setFocus(Qt::ShortcutFocusReason);}
void AssistantChatView::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);if(body_&&settingsDrawer_&&historyDrawer_){settingsDrawer_->setGeometry(body_->rect());historyDrawer_->setGeometry(body_->rect());RenderMessages();}}
void AssistantChatView::RefreshIcons()
{
    const auto icon=[this](const QString& resource,bool send=false){return theme_contrast::ThemedIcon(resource,palette().color(send?QPalette::HighlightedText:QPalette::ButtonText),palette().color(QPalette::Disabled,QPalette::ButtonText));};
    new_->setIcon(icon(QStringLiteral(":/okuflow/icons/chat-new.svg")));
    send_->setIcon(icon(QStringLiteral(":/okuflow/icons/chat-send.svg"),true));
    attachment_->setIcon(icon(QStringLiteral(":/okuflow/icons/camera.svg")));
    if(auto* history=header_->findChild<QToolButton*>(QStringLiteral("chatHistory")))history->setIcon(icon(QStringLiteral(":/okuflow/icons/chat-history.svg")));
    if(auto* settings=header_->findChild<QToolButton*>(QStringLiteral("chatSettings")))settings->setIcon(icon(QStringLiteral(":/okuflow/icons/chat-settings.svg")));
}
void AssistantChatView::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if(event->type()==QEvent::LanguageChange||event->type()==QEvent::PaletteChange||event->type()==QEvent::LayoutDirectionChange) {
        RetranslateWidgetTree(this);if(header_&&header_->parentWidget()!=this)RetranslateWidgetTree(header_);
        if(transcript_)RenderMessages();if(new_&&send_)RefreshIcons();
    }
}
bool AssistantChatView::eventFilter(QObject* watched,QEvent* event)
{
    if(transcript_ && watched==transcript_->viewport() && event->type()==QEvent::Resize && !renderQueued_) {
        renderQueued_=true;
        QTimer::singleShot(0,this,[this] {renderQueued_=false;RenderMessages();});
    }
    if(watched==composer_&&(event->type()==QEvent::ShortcutOverride||event->type()==QEvent::KeyPress)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if((key->key()==Qt::Key_Return||key->key()==Qt::Key_Enter)&&key->modifiers()==Qt::NoModifier) {
            key->accept();if(event->type()==QEvent::ShortcutOverride)return true;
            if(!key->isAutoRepeat()&&send_->isEnabled())send_->click();return true;
        }
        if(key->key()==Qt::Key_Escape&&key->modifiers()==Qt::NoModifier) {
            key->accept();if(event->type()==QEvent::ShortcutOverride)return true;
            if(!composer_->toPlainText().isEmpty())composer_->clear();else emit EscapeRequested();return true;
        }
    }
    return QWidget::eventFilter(watched,event);
}
} // namespace okuflow
#endif

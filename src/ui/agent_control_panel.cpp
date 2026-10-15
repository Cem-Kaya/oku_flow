#ifdef _WIN32
#include "okuflow/ui/agent_control_panel.hpp"
#include <QJsonArray>
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/live_status_text.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextDocument>
#include <QTimer>
#include <QTime>
#include <QVBoxLayout>
#include <algorithm>

namespace okuflow {
namespace {
class CheckLabel final : public QLabel {
public:
    CheckLabel(const QString& text, QCheckBox* check) : QLabel(text), check_(check) {}
protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && check_->isEnabled()) {
            check_->click(); event->accept(); return;
        }
        QLabel::mouseReleaseEvent(event);
    }
private:
    QCheckBox* check_;
};
QLabel* Text(QVBoxLayout* layout, const QString& source, const char* name = nullptr)
{
    auto* label = new QLabel(source);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    if (name) label->setObjectName(QString::fromLatin1(name));
    layout->addWidget(label);
    return label;
}
QCheckBox* Check(QVBoxLayout* layout, const QString& source, const char* name)
{
    // Checkbox text does not wrap in Qt. Keep its accessible name and put the
    // visible copy in a wrapping buddy label, so 360 px inspectors stay usable.
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0,0,0,0);
    auto* check = new QCheckBox;
    check->setMinimumSize(44,44);
    check->setObjectName(QString::fromLatin1(name));
    check->setAccessibleName(source);
    auto* label = new CheckLabel(source,check);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setBuddy(check);
    rowLayout->addWidget(check, 0, Qt::AlignTop);
    rowLayout->addWidget(label, 1);
    layout->addWidget(row);
    return check;
}
QPushButton* Button(QVBoxLayout* layout, const QString& source, const char* name)
{
    auto* button = new QPushButton(source);
    button->setObjectName(QString::fromLatin1(name));
    button->setMinimumHeight(44);
    button->setAutoDefault(false);
    button->setDefault(false);
    layout->addWidget(button);
    return button;
}
QString Ui(const char* source) { return TranslateUi(QString::fromUtf8(source)); }
QString NativeChangeSummary(const QJsonObject& record)
{
    // Only canonical app-normalized scalar fields get visible labels. Raw
    // request strings, paths and instructions remain in collapsed Details.
    const auto changes = record.value("normalizedChanges").toObject();
    QStringList fields;
    const auto boolean = [&](const char* id, const char* label) {
        const auto value = changes.value(QLatin1StringView(id));
        if (value.isBool()) fields.append(Ui(label) + QStringLiteral(": ") + Ui(value.toBool() ? "On" : "Off"));
    };
    const auto number = [&](const char* id, const char* label) {
        const auto value = changes.value(QLatin1StringView(id));
        const QString translated = Ui(label);
        if (value.isDouble()) fields.append(translated
            + (translated.endsWith(QLatin1Char(':')) ? QStringLiteral(" ") : QStringLiteral(": "))
            + QLocale().toString(value.toDouble(), 'g', 3));
    };
    boolean("image.superResolution.enabled", "NVIDIA Super Resolution");
    number("image.superResolution.strength", "Strength:");
    boolean("view.zoom.enabled", "Zoom");
    number("view.zoom.amount", "Zoom");
    number("image.contrast", "Contrast");
    number("image.brightness", "Brightness");
    boolean("image.textClarity.enabled", "Text Clarity");
    boolean("image.stabilization.enabled", "Stabilize Image");
    return fields.join(QStringLiteral("; ")).left(512);
}
QString ActionQuestion(const QJsonObject& record)
{
    const QString tool=record.value("tool").toString();
    if(record.value("kind").toString()=="frame-grant"||tool=="view.observe"||tool=="view_observe")return Ui("Send this camera view to the selected provider?");
    if(tool=="capture.photo.take"||tool=="capture_photo_take")return Ui("Take a photo of this view?");
    if(tool=="capture.recording.start"||tool=="capture_recording_start")return Ui("Start recording this view?");
    if(tool=="capture.recording.stop"||tool=="capture_recording_stop") {
        const auto time=record.value("startedAt").toString();
        const QTime parsed=QTime::fromString(time,QStringLiteral("HH:mm"));
        const QString display=parsed.isValid()?QLocale().toString(parsed,QLocale::ShortFormat):time;
        return time.isEmpty()?Ui("Stop this recording?"):Ui("Stop the recording started at %1?").arg(display);
    }
    if(tool=="camera.orientation.set"||tool=="camera_orientation_set")return Ui("Rotate the camera view to %1°?")
        .arg(record.value("arguments").toObject().value("degreesClockwise").toInt());
    if(tool=="camera.select"||tool=="camera_select")return Ui("Switch camera source?");
    if(tool=="camera.settings.set"||tool=="camera_settings_set")return Ui("Change camera settings?");
    if(tool=="recording.settings.set"||tool=="recording_settings_set") {
        const auto preferences=Ui("Change recording preferences?");
        if(record.value("sideEffects").toArray().contains("recording-stop")) {
            auto stop=record;stop.insert("tool","capture.recording.stop");
            return ActionQuestion(stop)+QLatin1Char('\n')+preferences;
        }
        return preferences;
    }
    if(tool=="assistant.preferences.set"||tool=="assistant_preferences_set")
        return Ui("Change Assistant provider, model or instructions?");
    if(tool=="notes.preferences.set"||tool=="notes_preferences_set")return Ui("Change notes and output settings?");
    if(tool=="annotation.preferences.set"||tool=="annotation_preferences_set")
        return record.value("sideEffects").toArray().contains("photo-save")?
            Ui("Change drawing settings and save a photo?"):Ui("Change drawing settings?");
    return Ui("Review this action?");
}
}

AgentControlPanel::AgentControlPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("agentControlPanel"));
    auto* layout = new QVBoxLayout(this);
    photos_ = Check(layout, QStringLiteral("Allow photos this session"), "agentPhotoGrant");
    recording_ = Check(layout, QStringLiteral("Allow the Assistant to start and stop recordings it started"), "agentRecordingGrant");
    Text(layout, QStringLiteral("Ask permission before recording a lecture or other people."));
    status_ = Text(layout, QStringLiteral("Assistant control is off"), "agentPanelStatus");
    approvalRow_ = new QWidget;
    approvalRow_->setObjectName(QStringLiteral("agentApprovalRow"));
    approvalRow_->setAttribute(Qt::WA_StyledBackground);
    approvalRow_->setStyleSheet(QStringLiteral("QWidget#agentApprovalRow { background:palette(base); border:1px solid palette(mid); border-radius:6px; } QLabel#agentApprovalSummary { font-size:20px; } QPushButton { font-size:16px; }"));
    auto* approvals = new QGridLayout(approvalRow_);approvals->setContentsMargins(12,6,12,6);approvals->setHorizontalSpacing(8);approvals->setVerticalSpacing(4);
    auto* role=new QLabel(QStringLiteral("Assistant"));role->setTextFormat(Qt::PlainText);approvals->addWidget(role,0,0);
    approvalExpiry_=new QLabel;approvalExpiry_->setObjectName(QStringLiteral("agentApprovalExpiry"));approvalExpiry_->setTextFormat(Qt::PlainText);approvalExpiry_->setWordWrap(true);approvalExpiry_->setMinimumWidth(110);approvalExpiry_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);approvals->addWidget(approvalExpiry_,0,1);
    auto* details=new QPushButton(QStringLiteral("Details"));details->setObjectName(QStringLiteral("agentApprovalDetails"));
    details->setAutoDefault(false);details->setDefault(false);details->setCheckable(true);details->setMinimumHeight(44);approvals->addWidget(details,0,2);
    approvalSummary_=new QLabel;approvalSummary_->setObjectName(QStringLiteral("agentApprovalSummary"));approvalSummary_->setTextFormat(Qt::PlainText);approvalSummary_->setWordWrap(true);
    QFont questionFont=approvalSummary_->font();questionFont.setPixelSize(20);approvalSummary_->setFont(questionFont);approvalSummary_->setStyleSheet(QStringLiteral("font-size:20px;"));
    QFont expiryFont=approvalExpiry_->font();expiryFont.setPixelSize(16);approvalExpiry_->setFont(expiryFont);approvalExpiry_->setStyleSheet(QStringLiteral("font-size:16px;"));
    approvalSummary_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);approvals->addWidget(approvalSummary_,1,0);
    allow_=new QPushButton(QStringLiteral("&Allow once"));allow_->setObjectName(QStringLiteral("agentApprovalAllow"));
    deny_=new QPushButton(QStringLiteral("&Deny"));deny_->setObjectName(QStringLiteral("agentApprovalDeny"));
    for(auto* button:{allow_,deny_}){button->setMinimumHeight(44);button->setAutoDefault(false);button->setDefault(false);}
    approvals->addWidget(allow_,1,1);approvals->addWidget(deny_,1,2);approvals->setColumnStretch(0,1);
    approvalDetails_=new QWidget;auto* detailLayout=new QVBoxLayout(approvalDetails_);
    approvalArguments_=new QPlainTextEdit;approvalArguments_->setObjectName(QStringLiteral("agentApprovalArguments"));approvalArguments_->setReadOnly(true);
    approvalArguments_->setAccessibleName(QStringLiteral("Exact action arguments"));approvalArguments_->setMaximumHeight(140);
    detailLayout->addWidget(approvalArguments_);approvals->addWidget(approvalDetails_,2,0,1,3);approvalDetails_->hide();
    connect(details,&QPushButton::toggled,approvalDetails_,&QWidget::setVisible);
    for (auto* widget : {static_cast<QWidget*>(approvalArguments_), static_cast<QWidget*>(allow_), static_cast<QWidget*>(deny_),static_cast<QWidget*>(details)}) widget->installEventFilter(this);
    connect(allow_, &QPushButton::clicked, this, [this] { ResolvePending(true); });
    connect(deny_, &QPushButton::clicked, this, [this] { ResolvePending(false); });
    layout->addWidget(approvalRow_);
    approvalRow_->installEventFilter(this);
    approvalRow_->hide();

    routingRow_ = new QWidget;
    auto* routing = new QVBoxLayout(routingRow_);
    routingMessage_ = Text(routing, QStringLiteral("Controlling OkuFlow needs its own conversation."), "agentRoutingMessage");
    startControl_ = Button(routing, QStringLiteral("Start control conversation"), "agentStartConversation");
    routingTest_ = Button(routing, QStringLiteral("Test endpoint tool support now"), "agentRoutingCapabilityTest");
    auto* without = Button(routing, QStringLiteral("Send without control"), "agentSendWithoutControl");
    connect(startControl_, &QPushButton::clicked, this, &AgentControlPanel::startControlConversationRequested);
    connect(routingTest_, &QPushButton::clicked, this, &AgentControlPanel::capabilityTestRequested);
    routingTest_->hide();
    connect(without, &QPushButton::clicked, this, &AgentControlPanel::sendWithoutControlRequested);
    layout->addWidget(routingRow_);
    routingRow_->hide();

    auto* showExternal = Button(layout, QStringLiteral("External control permissions"), "agentExternalDisclosure");
    showExternal->setCheckable(true);
    externalSection_ = new QWidget;
    auto* ext = new QVBoxLayout(externalSection_);
    connection_ = Text(ext, QStringLiteral("No external client connected"), "agentExternalIdentity");
    reviewLease_ = Button(ext, QStringLiteral("Review lease permissions"), "agentReviewLease");
    bundle_ = new QGroupBox(QStringLiteral("Lease permissions"));
    auto* grants = new QVBoxLayout(bundle_);
    auto* view = Check(grants, QStringLiteral("Adjust view (required)"), "agentLeaseView");
    view->setChecked(true);
    view->setEnabled(false);
    leaseFrames_ = Check(grants, QStringLiteral("Send camera views (up to 6 per lease)"), "agentLeaseFrames");
    leaseSource_ = Check(grants, QStringLiteral("Orientation and camera source (never during recording)"), "agentLeaseSource");
    leasePhotos_ = Check(grants, QStringLiteral("Take photos (within the lease action budget)"), "agentLeasePhotos");
    leaseRecording_ = Check(grants, QStringLiteral("Start and stop recordings it started (at least 30 seconds remaining to start)"), "agentLeaseRecording");
    Text(grants, QStringLiteral("Ask permission before recording a lecture or other people."));
    Text(grants, QStringLiteral("View changes: up to 60 per lease. Permissions cannot be widened during a lease; revoke it and issue a new one."));
    issueLease_ = Button(grants, QStringLiteral("Approve lease"), "agentIssueLease");
    ext->addWidget(bundle_);
    bundle_->hide();
    leaseStatus_ = Text(ext, QStringLiteral("No active lease"), "agentLeaseStatus");
    revokeLease_ = Button(ext, QStringLiteral("Revoke lease"), "agentRevokeLease");
    connect(showExternal, &QPushButton::toggled, externalSection_, &QWidget::setVisible);
    connect(reviewLease_, &QPushButton::clicked, this, [this] {
        for (auto* grant : {leaseFrames_, leaseSource_, leasePhotos_, leaseRecording_}) grant->setChecked(false);
        bundle_->show(); // Only an explicit user activation opens the bundle.
    });
    connect(issueLease_, &QPushButton::clicked, this, [this] {
        if (connectionId_.isEmpty() || !externalEnabled_ || !leaseRecord_.isEmpty()) return;
        const QJsonObject grants{{"frames",leaseFrames_->isChecked()}, {"source",leaseSource_->isChecked()},
                                 {"photos",leasePhotos_->isChecked()}, {"recording",leaseRecording_->isChecked()}};
        issueLease_->setEnabled(false);
        emit approveLeaseRequested(connectionId_, grants);
    });
    connect(revokeLease_, &QPushButton::clicked, this, &AgentControlPanel::revokeLeaseRequested);
    layout->addWidget(externalSection_);
    externalSection_->hide();

    auto* test = Button(layout, QStringLiteral("Test endpoint tool support now"), "agentCapabilityTest");
    capability_ = Text(layout, {}, "agentCapabilityStatus");
    connect(test, &QPushButton::clicked, this, &AgentControlPanel::capabilityTestRequested);
    transcript_ = new QPlainTextEdit;
    transcript_->setObjectName(QStringLiteral("agentActionTranscript"));
    transcript_->setReadOnly(true);
    transcript_->setAccessibleName(QStringLiteral("Assistant action transcript"));
    transcript_->document()->setMaximumBlockCount(128);
    transcript_->setMinimumHeight(120);
    auto* activity=Button(layout,QStringLiteral("Activity log"),"agentActivityDisclosure");activity->setCheckable(true);
    layout->addWidget(transcript_);transcript_->hide();connect(activity,&QPushButton::toggled,transcript_,&QWidget::setVisible);
    auto* about=Button(layout,QStringLiteral("About control"),"agentAboutDisclosure");about->setCheckable(true);
    auto* aboutContent=new QWidget;auto* aboutLayout=new QVBoxLayout(aboutContent);
    Text(aboutLayout,QStringLiteral("When enabled, the Assistant can adjust the view. Camera images and captures need separate permission. Stop and undo with Ctrl+Shift+U."));
    Text(aboutLayout,QStringLiteral("External control lasts at most 5 minutes and ends after 60 seconds without activity. Disconnect the MCP client or choose Revoke lease to stop it."));
    Text(aboutLayout,QStringLiteral("This explicit test sends a synthetic tool request to the configured endpoint. It sends no camera image and changes no settings."));
    layout->addWidget(aboutContent);aboutContent->hide();connect(about,&QPushButton::toggled,aboutContent,&QWidget::setVisible);
    for (auto* grant : {photos_, recording_}) connect(grant, &QCheckBox::toggled, this, [this] {
        emit sessionGrantsChanged(framesGranted_, photos_->isChecked(), recording_->isChecked());
    });
    countdown_ = new QTimer(this);
    countdown_->setInterval(1000);
    connect(countdown_, &QTimer::timeout, this, &AgentControlPanel::UpdateCountdowns);
    countdown_->start();
    UpdateAvailability();
    Retranslate();
}

void AgentControlPanel::SetEnabled(bool embedded, bool external)
{
    embeddedEnabled_=embedded;externalEnabled_=external;
    if (!embedded) SetSessionGrants(false, false, false);
    if (!external) bundle_->hide();
    UpdateAvailability();
}
void AgentControlPanel::SetSessionGrants(bool frames, bool photos, bool recording)
{
    const QSignalBlocker b(photos_), c(recording_);
    framesGranted_=embeddedEnabled_ && frames;
    photos_->setChecked(embeddedEnabled_ && photos);
    recording_->setChecked(embeddedEnabled_ && recording);
}
void AgentControlPanel::UpdateAvailability()
{
    for (auto* grant : {photos_, recording_}) grant->setEnabled(embeddedEnabled_);
    const bool active = !leaseRecord_.isEmpty();
    reviewLease_->setEnabled(externalEnabled_ && !connectionId_.isEmpty() && !active);
    issueLease_->setEnabled(reviewLease_->isEnabled());
    revokeLease_->setEnabled(active);
    for (auto* grant : {leaseFrames_, leaseSource_, leasePhotos_, leaseRecording_}) grant->setEnabled(!active);
}
void AgentControlPanel::SetPendingApproval(const QJsonObject& record)
{
    const QString id = record.value("approvalId").toString();
    const auto args = QJsonDocument(record.value("arguments").toObject()).toJson(QJsonDocument::Indented);
    const auto changes = record.value("normalizedChanges");
    const auto reviewed = QJsonDocument(changes.isObject() ? changes.toObject() : record.value("arguments").toObject()).toJson(QJsonDocument::Indented);
    if (id.isEmpty() || id.size() > 256 || args.size() > 16384 || reviewed.size() > 30720) { ClearPendingApproval(); return; }
    const bool distinct = id != approvalId_;
    approvalId_ = id;
    approvalRecord_ = record;
    approvalRemainingMs_ = std::clamp<qint64>(record.value("expiresInMs").toInteger(300000), 0, 300000);
    approvalClock_.restart();
    const auto reviewText = record.value("tool").toString()+QStringLiteral("\n")+record.value("targetDisplay").toString().left(256)+QStringLiteral("\n")+record.value("targetIdentity").toString()+QStringLiteral("\n")+QString::fromUtf8(reviewed);
    // Lease/countdown refreshes must not replace the document a user is reading.
    if (distinct || approvalArguments_->toPlainText() != reviewText)
        approvalArguments_->setPlainText(reviewText);
    if(distinct){approvalDetails_->hide();approvalRow_->findChild<QPushButton*>(QStringLiteral("agentApprovalDetails"))->setChecked(false);}
    approvalRow_->show(); // Never raise, activate, focus, or open a dialog.
    UpdateCountdowns();
    if (distinct) {
        ++approvalSequence_;
        const QString message = Ui("Permission request %1: %2").arg(approvalSequence_).arg(ActionQuestion(record));
        AppendAction(message, true);
    }
}
void AgentControlPanel::ClearPendingApproval()
{
    approvalId_.clear(); approvalRecord_ = {};
    approvalRow_->hide();
    approvalArguments_->clear();
}
void AgentControlPanel::FocusPendingApproval()
{
    if (!approvalId_.isEmpty()) deny_->setFocus(Qt::ShortcutFocusReason);
}
void AgentControlPanel::ResolvePending(bool allow)
{
    if (approvalId_.isEmpty()) return;
    if (allow && approvalClock_.elapsed() >= approvalRemainingMs_) { UpdateCountdowns(); return; }
    const QString id = approvalId_;
    ClearPendingApproval(); // Duplicate activation cannot emit twice.
    emit approvalResolved(id, allow);
}
bool AgentControlPanel::eventFilter(QObject* watched, QEvent* event)
{
    if(watched==approvalRow_ && event->type()==QEvent::Resize) {
        const bool compact=approvalRow_->width()<460;
        if(approvalRow_->property("compactApprovalLayout").toBool()!=compact) {
            approvalRow_->setProperty("compactApprovalLayout",compact);
            auto* grid=qobject_cast<QGridLayout*>(approvalRow_->layout());
            grid->removeWidget(approvalSummary_);grid->removeWidget(allow_);grid->removeWidget(deny_);grid->removeWidget(approvalDetails_);
            grid->addWidget(approvalSummary_,1,0,1,compact?3:1);
            grid->addWidget(allow_,compact?2:1,compact?0:1,1,compact?2:1);
            grid->addWidget(deny_,compact?2:1,2);
            grid->addWidget(approvalDetails_,compact?3:2,0,1,3);
        }
    }
    if (!approvalId_.isEmpty() && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            ResolvePending(false); key->accept(); return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void AgentControlPanel::SetExternalConnection(const QString& id, const QString& identity)
{
    if (connectionId_ != id) {
        bundle_->hide();
        for (auto* grant : {leaseFrames_, leaseSource_, leasePhotos_, leaseRecording_}) grant->setChecked(false);
    }
    connectionId_ = id;
    connectionIdentity_ = identity.left(512);
    connection_->setText(id.isEmpty() ? Ui("No external client connected") : Ui("Connected client: %1").arg(connectionIdentity_));
    UpdateAvailability();
}
void AgentControlPanel::SetExternalLease(const QJsonObject& status)
{
    leaseRecord_ = status.value("active").toBool() && status.value("ownerKind").toString() == "external-lease" ? status : QJsonObject{};
    leaseAbsoluteMs_ = std::max<qint64>(0, status.value("remainingAbsoluteMs").toInteger());
    leaseIdleMs_ = std::max<qint64>(0, status.value("remainingIdleMs").toInteger());
    leaseClock_.restart();
    if (!leaseRecord_.isEmpty()) {
        const auto grants = leaseRecord_.value("grants").toObject();
        leaseFrames_->setChecked(grants.value("frames").toBool());
        leaseSource_->setChecked(grants.value("source").toBool());
        leasePhotos_->setChecked(grants.value("photos").toBool());
        leaseRecording_->setChecked(grants.value("recording").toBool());
    } else bundle_->hide();
    UpdateCountdowns(); UpdateAvailability();
}
void AgentControlPanel::UpdateCountdowns()
{
    if (!approvalId_.isEmpty()) {
        const qint64 remaining = std::max<qint64>(0, approvalRemainingMs_ - approvalClock_.elapsed());
        const auto record = approvalRecord_;
        const QString target=record.value("targetDisplay").toString().left(256);
        const QString changes = NativeChangeSummary(record);
        approvalSummary_->setText(ActionQuestion(record)
            + (changes.isEmpty() ? QString{} : QStringLiteral("\n") + changes)
            + (target.isEmpty() ? QString{} : QStringLiteral("\n") + target));
        approvalExpiry_->setText(Ui("Expires in %1 seconds").arg((remaining+999)/1000));
        allow_->setEnabled(remaining > 0);
    }
    if (leaseRecord_.isEmpty()) { leaseStatus_->setText(Ui("No active lease")); return; }
    const qint64 elapsed = leaseClock_.elapsed();
    leaseStatus_->setText(Ui("External control: %1 seconds remaining; idle timeout %2 seconds. View changes: %3; camera views: %4; photos: %5.")
        .arg((std::max<qint64>(0,leaseAbsoluteMs_-elapsed)+999)/1000)
        .arg((std::max<qint64>(0,leaseIdleMs_-elapsed)+999)/1000)
        .arg(leaseRecord_.value("remainingMutations").toInt()).arg(leaseRecord_.value("remainingObservations").toInt())
        .arg(leaseRecord_.value("remainingMutations").toInt()));
}
void AgentControlPanel::AppendAction(const QString& text, bool assertive)
{
    // Bounded, pixel-free memory only. QPlainTextEdit never interprets HTML.
    transcript_->appendPlainText(text.left(2048));
    while (transcript_->toPlainText().size() > 32768) {
        QTextCursor cursor(transcript_->document());
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    SetStatus(text.left(2048), assertive, !assertive);
    emit actionReceiptAdded(text.left(2048),assertive);
}
void AgentControlPanel::SetStatus(const QString& text, bool assertive, bool coalesce)
{
    SetLiveText(status_, text.left(2048), LivePoliteness::kSilent, Ui("Assistant control status"));
    emit statusAnnouncementRequested(text.left(2048), assertive, coalesce);
}
void AgentControlPanel::SetCapabilityStatus(const QString& text)
{
    capabilityText_=text.left(2048);
    SetLiveText(capability_,capabilityText_,LivePoliteness::kSilent);
    if (!routingTest_->isHidden())
        SetLiveText(routingMessage_,capabilityText_.isEmpty()?QStringLiteral("Tool support unavailable"):capabilityText_,LivePoliteness::kSilent);
}
void AgentControlPanel::SetRoutingNotice(bool visible, bool canStart)
{
    // Keyboard focus stays in the row when its offered action changes.
    const bool focused = routingRow_->isAncestorOf(QApplication::focusWidget());
    startControl_->setVisible(canStart);
    routingTest_->setVisible(!canStart);
    const QString message = canStart ? QStringLiteral("Controlling OkuFlow needs its own conversation.")
        : capabilityText_.isEmpty() ? QStringLiteral("Tool support unavailable") : capabilityText_;
    SetLiveText(routingMessage_,message,LivePoliteness::kSilent);
    routingRow_->setVisible(visible);
    if (visible && focused) (canStart ? startControl_ : routingTest_)->setFocus(Qt::OtherFocusReason);
    if (visible) emit statusAnnouncementRequested(message,true,false);
}
void AgentControlPanel::RefreshRoutingNotice(bool canStart)
{
    if (routingRow_->isHidden() || startControl_->isHidden() != canStart) return;
    SetRoutingNotice(true,canStart);
}
void AgentControlPanel::RequestStopAndUndo() { emit stopAndUndoRequested(); }
void AgentControlPanel::Retranslate() { RetranslateWidgetTree(this); UpdateCountdowns(); SetExternalConnection(connectionId_, connectionIdentity_); }
void AgentControlPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) Retranslate();
}
} // namespace okuflow
#endif

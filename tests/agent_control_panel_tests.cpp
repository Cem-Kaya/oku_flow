#include "okuflow/ui/agent_control_panel.hpp"
#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/common/visible_chat_session.hpp"
#include "okuflow/ui/color_scheme_picker.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include <QLocale>
#include <QComboBox>
#include <QSpinBox>
#include <QJsonArray>
#include <QTextBrowser>
#include <QScrollBar>
#include <QTabWidget>
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextDocument>
#include <QtTest>

using okuflow::AgentControlPanel;
class AgentControlPanelTests final : public QObject {
    Q_OBJECT
private slots:
    void consentOnlyFromUserActivation();
    void exactApprovalPlainTextAndSingleUse();
    void expiredApprovalCannotAllow();
    void approvalShowsNormalizedDependenciesAndTarget();
    void approvalRefreshPreservesDetailsReadingState();
    void leaseBundleDefaultsAndCannotWiden();
    void boundedTranscriptAndExplicitEndpointTest();
    void floatingRoutingRefusalPreservesQuestion();
    void sharedChatDraftAndKeyboard();
    void escapedMessagesSelectionAndReadIsolation();
    void orientationUsesCanonicalDegrees();
    void customRecipeAbsenceRestoresSilently();
};
namespace {
template<class T> T* Find(AgentControlPanel& panel, const char* name)
{
    return panel.findChild<T*>(QString::fromLatin1(name));
}
QJsonObject Approval(const QString& id, qint64 remaining = 300000)
{
    return {{"approvalId",id},{"tool","capture.photo.take"},{"arguments",QJsonObject{{"caption","<img src=x onerror=alert(1)>"}}},
            {"kind","action"},{"targetIdentity","opaque-session"},{"expiresInMs",remaining}};
}
}
void AgentControlPanelTests::consentOnlyFromUserActivation()
{
    AgentControlPanel panel;
    QSignalSpy enabled(&panel,&AgentControlPanel::enabledChanged);
    QSignalSpy grants(&panel,&AgentControlPanel::sessionGrantsChanged);
    QVERIFY(!Find<QCheckBox>(panel,"agentControlEnabled"));
    QVERIFY(!Find<QCheckBox>(panel,"externalMcpEnabled"));
    QVERIFY(!Find<QCheckBox>(panel,"agentFrameGrant"));
    panel.SetEnabled(true,true);
    panel.SetSessionGrants(true,true,true);
    QCOMPARE(enabled.size(),0);
    QCOMPARE(grants.size(),0);
    Find<QCheckBox>(panel,"agentPhotoGrant")->click();
    QCOMPARE(grants.size(),1);
    QVERIFY(grants.first().at(0).toBool());
    QVERIFY(!grants.first().at(1).toBool());
    panel.SetEnabled(false,false);
    QVERIFY(!Find<QCheckBox>(panel,"agentPhotoGrant")->isChecked());
    QCOMPARE(enabled.size(),0);
    QCOMPARE(grants.size(),1); // Incoming revocation never mints a grant.
}
void AgentControlPanelTests::exactApprovalPlainTextAndSingleUse()
{
    AgentControlPanel panel;
    QSignalSpy decisions(&panel,&AgentControlPanel::approvalResolved);
    QSignalSpy announcements(&panel,&AgentControlPanel::statusAnnouncementRequested);
    panel.SetPendingApproval(Approval("id-one"));
    QCOMPARE(decisions.size(),0);
    QVERIFY(panel.hasPendingApproval());
    QCOMPARE(Find<QLabel>(panel,"agentApprovalSummary")->textFormat(),Qt::PlainText);
    QVERIFY(Find<QPlainTextEdit>(panel,"agentApprovalArguments")->toPlainText().contains("<img src=x"));
    QCOMPARE(Find<QLabel>(panel,"agentApprovalSummary")->text(),QStringLiteral("Take a photo of this view?"));
    QVERIFY(!Find<QPlainTextEdit>(panel,"agentApprovalArguments")->isVisibleTo(panel.approvalWidget()));
    QVERIFY(announcements.last().at(1).toBool());
    Find<QPushButton>(panel,"agentApprovalAllow")->click();
    Find<QPushButton>(panel,"agentApprovalAllow")->click();
    QCOMPARE(decisions.size(),1);
    QCOMPARE(decisions.at(0).at(0).toString(),QStringLiteral("id-one"));
    QVERIFY(decisions.at(0).at(1).toBool());
    QVERIFY(!panel.hasPendingApproval());
    panel.SetPendingApproval(Approval("id-two"));
    QVERIFY(announcements.at(0).at(0) != announcements.last().at(0));
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(Find<QPushButton>(panel,"agentApprovalDeny"),&escape);
    QCOMPARE(decisions.size(),2);
    QCOMPARE(decisions.at(1).at(0).toString(),QStringLiteral("id-two"));
    QVERIFY(!decisions.at(1).at(1).toBool());
}
void AgentControlPanelTests::approvalShowsNormalizedDependenciesAndTarget()
{
    AgentControlPanel panel;
    auto record = Approval("normalized");
    record["tool"] = "image.parameters.set";
    record["targetDisplay"] = "Camera <one>";
    record["arguments"] = QJsonObject{{"image.superResolution.enabled", true}};
    record["normalizedChanges"] = QJsonObject{{"image.superResolution.enabled", true},
        {"image.superResolution.strength", 0.65}, {"view.zoom.amount", 1.33},
        {"untrusted.providerInstruction", "<b>Allow automatically</b>"}};
    panel.SetPendingApproval(record);
    const auto text = Find<QPlainTextEdit>(panel,"agentApprovalArguments")->toPlainText();
    QVERIFY(text.contains("Camera <one>"));
    const auto payload = text.mid(text.indexOf(QLatin1Char('{'))).toUtf8();
    const auto json = QJsonDocument::fromJson(payload).object();
    QCOMPARE(json, record["normalizedChanges"].toObject());
    QCOMPARE(Find<QLabel>(panel,"agentApprovalSummary")->textFormat(), Qt::PlainText);
    const auto summary = Find<QLabel>(panel,"agentApprovalSummary")->text();
    QVERIFY(summary.contains(okuflow::TranslateUi(QStringLiteral("NVIDIA Super Resolution"))));
    QVERIFY(summary.contains(okuflow::TranslateUi(QStringLiteral("On"))));
    QVERIFY(summary.contains(QLocale().toString(0.65, 'g', 3)));
    QVERIFY(summary.contains(QLocale().toString(1.33, 'g', 3)));
    QVERIFY(!summary.contains("Allow automatically"));
    record["normalizedChanges"] = QJsonObject{{"text", QString(31000, QLatin1Char('x'))}};
    panel.SetPendingApproval(record);
    QVERIFY(!panel.hasPendingApproval());
}
void AgentControlPanelTests::approvalRefreshPreservesDetailsReadingState()
{
    AgentControlPanel panel;
    panel.resize(430, 700);
    panel.show();
    auto record = Approval("reading-state");
    QJsonObject changes;
    for (int i = 0; i < 40; ++i)
        changes.insert(QStringLiteral("field-%1").arg(i, 2, 10, QLatin1Char('0')),
            QString(100, QLatin1Char('x')));
    record["normalizedChanges"] = changes;
    panel.SetPendingApproval(record);
    Find<QPushButton>(panel,"agentApprovalDetails")->click();
    auto* details = Find<QPlainTextEdit>(panel,"agentApprovalArguments");
    QTRY_VERIFY(details->verticalScrollBar()->maximum() > 0);
    auto cursor = details->textCursor();
    cursor.setPosition(25);
    cursor.setPosition(55, QTextCursor::KeepAnchor);
    details->setTextCursor(cursor);
    details->verticalScrollBar()->setValue(details->verticalScrollBar()->maximum() / 2);
    const int scroll = details->verticalScrollBar()->value();
    const int revision = details->document()->revision();
    QSignalSpy textChanges(details, &QPlainTextEdit::textChanged);
    for (int refresh = 0; refresh < 4; ++refresh) {
        record["expiresInMs"] = 299000 - refresh * 250;
        panel.SetPendingApproval(record);
    }
    QCOMPARE(textChanges.size(), 0);
    QCOMPARE(details->document()->revision(), revision);
    QCOMPARE(details->textCursor().anchor(), cursor.anchor());
    QCOMPARE(details->textCursor().position(), cursor.position());
    QCOMPARE(details->verticalScrollBar()->value(), scroll);
    QVERIFY(details->isVisible());
    // A changed exact payload must still replace the review text.
    record["targetDisplay"] = "Changed native target";
    panel.SetPendingApproval(record);
    QCOMPARE(textChanges.size(), 1);
    QVERIFY(details->toPlainText().contains("Changed native target"));
    record["approvalId"] = "different-request";
    panel.SetPendingApproval(record);
    QVERIFY(!details->isVisible());
}
void AgentControlPanelTests::expiredApprovalCannotAllow()
{
    AgentControlPanel panel;
    QSignalSpy decisions(&panel,&AgentControlPanel::approvalResolved);
    panel.SetPendingApproval(Approval("expired",0));
    QVERIFY(!Find<QPushButton>(panel,"agentApprovalAllow")->isEnabled());
    Find<QPushButton>(panel,"agentApprovalAllow")->click();
    QCOMPARE(decisions.size(),0);
    // Arguments outside the service bound fail closed rather than hide details.
    auto oversized = Approval("oversized");
    oversized["arguments"] = QJsonObject{{"text",QString(17000,QLatin1Char('x'))}};
    panel.SetPendingApproval(oversized);
    QVERIFY(!panel.hasPendingApproval());
}
void AgentControlPanelTests::leaseBundleDefaultsAndCannotWiden()
{
    AgentControlPanel panel;
    QSignalSpy leases(&panel,&AgentControlPanel::approveLeaseRequested);
    panel.SetEnabled(false,true);
    panel.SetExternalConnection("client-one","<b>Untrusted identity</b>");
    QVERIFY(Find<QWidget>(panel,"agentExternalIdentity")->parentWidget()->isHidden());
    QVERIFY(!Find<QWidget>(panel,"agentApprovalRow")->isVisible());
    Find<QPushButton>(panel,"agentExternalDisclosure")->click();
    Find<QPushButton>(panel,"agentReviewLease")->click();
    for (const auto* key : {"agentLeaseFrames","agentLeaseSource","agentLeasePhotos","agentLeaseRecording"}) QVERIFY(!Find<QCheckBox>(panel,key)->isChecked());
    QVERIFY(Find<QCheckBox>(panel,"agentLeaseView")->isChecked());
    QVERIFY(!Find<QCheckBox>(panel,"agentLeaseView")->isEnabled());
    Find<QCheckBox>(panel,"agentLeasePhotos")->click();
    Find<QPushButton>(panel,"agentIssueLease")->click();
    QCOMPARE(leases.size(),1);
    QCOMPARE(leases.at(0).at(0).toString(),QStringLiteral("client-one"));
    QCOMPARE(leases.at(0).at(1).toJsonObject(),(QJsonObject{{"frames",false},{"source",false},{"photos",true},{"recording",false}}));
    panel.SetExternalLease({{"active",true},{"ownerKind","external-lease"},{"remainingAbsoluteMs",300000},{"remainingIdleMs",60000},{"remainingMutations",60},{"remainingObservations",6}});
    QVERIFY(!Find<QPushButton>(panel,"agentReviewLease")->isEnabled());
    Find<QPushButton>(panel,"agentIssueLease")->click();
    QCOMPARE(leases.size(),1);
    QVERIFY(Find<QPushButton>(panel,"agentRevokeLease")->isEnabled());
}
void AgentControlPanelTests::boundedTranscriptAndExplicitEndpointTest()
{
    AgentControlPanel panel;
    QSignalSpy tests(&panel,&AgentControlPanel::capabilityTestRequested);
    QSignalSpy stop(&panel,&AgentControlPanel::stopAndUndoRequested);
    panel.SetCapabilityStatus("unavailable");
    QCOMPARE(tests.size(),0);
    Find<QPushButton>(panel,"agentCapabilityTest")->click();
    QCOMPARE(tests.size(),1);
    panel.RequestStopAndUndo();
    QCOMPARE(stop.size(),1);
    panel.AppendAction("<a href='https://invalid'>plain</a>");
    auto* text = Find<QPlainTextEdit>(panel,"agentActionTranscript");
    QVERIFY(text->toPlainText().contains("<a href="));
    for (int i=0;i<250;++i) panel.AppendAction(QString(2000,QLatin1Char('x')));
    QVERIFY(text->toPlainText().size() <= 32768);
    QVERIFY(text->document()->blockCount() <= 128);
}
void AgentControlPanelTests::floatingRoutingRefusalPreservesQuestion()
{
    okuflow::AssistiveOverlay overlay;
    auto* question=overlay.chatView()->composer();
    auto* ask=overlay.chatView()->sendButton();
    QSignalSpy sent(&overlay,&okuflow::AssistiveOverlay::QuestionSubmitted);
    connect(&overlay,&okuflow::AssistiveOverlay::QuestionSubmitted,&overlay,[&] {overlay.SetControlRoutingNotice(true,true);});
    question->setPlainText(QStringLiteral("make the board readable"));
    ask->click();
    QCOMPARE(sent.size(),1);
    QCOMPARE(overlay.QuestionDraft(),QStringLiteral("make the board readable"));
    QVERIFY(!overlay.agentControlPanel()->routingNoticeWidget()->isHidden());
    // A pure view keeps the draft until the app-owned model accepts it.
    overlay.SetChatSnapshot({{"conversationId","accepted"},{"draft",""},{"busy",true}});
    QVERIFY(question->toPlainText().isEmpty());
}
void AgentControlPanelTests::sharedChatDraftAndKeyboard()
{
    okuflow::VisibleChatSession model;
    okuflow::AssistantChatView main,floating;
    for(auto* view:{&main,&floating}) {
        connect(&model,&okuflow::VisibleChatSession::SnapshotChanged,view,&okuflow::AssistantChatView::SetChatSnapshot);
        connect(view,&okuflow::AssistantChatView::ChatDraftEdited,&model,&okuflow::VisibleChatSession::SetDraft);
        connect(view,&okuflow::AssistantChatView::ChatAttachmentChanged,&model,&okuflow::VisibleChatSession::SetAttachCurrentView);
        view->SetChatSnapshot(model.Snapshot());
    }
    QSignalSpy sends(&main,&okuflow::AssistantChatView::ChatSubmitted);
    main.composer()->setPlainText("one draft");
    QCOMPARE(floating.composer()->toPlainText(),QStringLiteral("one draft"));
    floating.attachmentCheckbox()->click();
    QVERIFY(main.attachCurrentView());
    QTest::keyClick(main.composer(),Qt::Key_Return);
    QCOMPARE(sends.size(),1);
    QCOMPARE(model.Draft(),QStringLiteral("one draft"));
    QKeyEvent repeat(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier,QString(),true,2);
    QApplication::sendEvent(main.composer(),&repeat);
    QCOMPARE(sends.size(),1);
    QTest::keyClick(main.composer(),Qt::Key_Return,Qt::ShiftModifier);
    QVERIFY(model.Draft().contains('\n'));
    const auto ticket=model.Admit(model.Draft(),okuflow::ChatBackendKind::Explain);
    QVERIFY(ticket.has_value());
    QVERIFY(main.composer()->toPlainText().isEmpty());
    QVERIFY(!main.sendButton()->isEnabled());
    QVERIFY(main.attachmentCheckbox()->isEnabled());
    floating.attachmentCheckbox()->click();
    QVERIFY(!main.attachCurrentView());
    QVERIFY(!main.attachmentCheckbox()->isEnabled());
    main.attachmentCheckbox()->click();
    QVERIFY(!model.AttachCurrentView());
    QTest::keyClick(main.composer(),Qt::Key_Return);
    QCOMPARE(sends.size(),1);
    QVERIFY(model.Complete(*ticket,"done"));
    QSignalSpy speech(&main,&okuflow::AssistantChatView::ChatReadAloudRequested);
    QCOMPARE(speech.size(),0);
    main.readAloudButton()->click();
    QCOMPARE(speech.size(),1);
    QCOMPARE(speech.first().at(1).toString(),QStringLiteral("done"));
}
void AgentControlPanelTests::escapedMessagesSelectionAndReadIsolation()
{
    okuflow::AssistiveOverlay overlay;
    auto* view=overlay.chatView();
    QJsonObject row{{"id","a"},{"role","assistant"},{"text","select me <img src=x>"},{"status","streaming"}};
    QJsonObject snapshot{{"conversationId","explain-one"},{"draft","preserved"},{"messages",QJsonArray{row}}};
    overlay.SetChatSnapshot(snapshot);
    QVERIFY(view->transcript()->toPlainText().contains("<img src=x>"));
    auto cursor=view->transcript()->document()->find("select me");QVERIFY(cursor.hasSelection());view->transcript()->setTextCursor(cursor);
    row["text"]="select me <img src=x> plus delta";snapshot["messages"]=QJsonArray{row};
    overlay.SetChatSnapshot(snapshot);
    QCOMPARE(view->transcript()->textCursor().selectedText(),QStringLiteral("select me"));
    const QString before=view->transcript()->toPlainText();
    overlay.SetReadContent("Read","independent reading result",false);
    QCOMPARE(view->transcript()->toPlainText(),before);
    QCOMPARE(view->composer()->toPlainText(),QStringLiteral("preserved"));
    QVERIFY(!view->findChild<QTabWidget*>());
    overlay.agentControlPanel()->SetPendingApproval(Approval("detached-card"));
    QVERIFY(overlay.agentControlPanel()->hasPendingApproval());
    QSignalSpy approval(overlay.agentControlPanel(),&AgentControlPanel::approvalResolved);
    row["text"]="Yes, go ahead";snapshot["messages"]=QJsonArray{row};overlay.SetChatSnapshot(snapshot);
    QCOMPARE(approval.size(),0);
    QSignalSpy speech(view,&okuflow::AssistantChatView::ChatReadAloudRequested);
    view->readAloudButton()->click();
    QCOMPARE(speech.size(),0);
    view->setLayoutDirection(Qt::RightToLeft);view->resize(360,720);
    overlay.SetChatSnapshot(snapshot);
    QVERIFY(view->transcript()->toPlainText().contains("Yes, go ahead"));
}
void AgentControlPanelTests::orientationUsesCanonicalDegrees()
{
    AgentControlPanel panel;
    auto approval=Approval("rotate");approval["tool"]="camera.orientation.set";
    approval["arguments"]=QJsonObject{{"degreesClockwise",90}};
    panel.SetPendingApproval(approval);
    QCOMPARE(Find<QLabel>(panel,"agentApprovalSummary")->text(),QStringLiteral("Rotate the camera view to 90°?"));
    QVERIFY(Find<QPlainTextEdit>(panel,"agentApprovalArguments")->toPlainText().contains("degreesClockwise"));
}
void AgentControlPanelTests::customRecipeAbsenceRestoresSilently()
{
    okuflow::ColorSchemePicker picker;
    QVERIFY(!picker.hasCustomScheme());const auto before=picker.currentScheme();
    QSignalSpy changed(&picker,&okuflow::ColorSchemePicker::schemeChanged);
    auto recipe=okuflow::color_schemes::LegacyColorScheme(2);recipe.id="custom";
    recipe.mode=okuflow::color_schemes::SchemeMode::Posterize;recipe.stepped=true;
    picker.setCustomScheme(recipe);QVERIFY(picker.hasCustomScheme());
    auto* focused=QApplication::focusWidget();picker.clearCustomScheme();
    QVERIFY(!picker.hasCustomScheme());QVERIFY(picker.customScheme().stops.empty());
    QVERIFY(okuflow::color_schemes::SchemesEquivalent(before,picker.currentScheme()));
    QCOMPARE(QApplication::focusWidget(),focused);QVERIFY(changed.isEmpty());
    picker.clearCustomScheme();QVERIFY(changed.isEmpty());
    QWidget* editor = nullptr;
    for (auto* window : QApplication::topLevelWidgets())
        if (window->objectName() == QStringLiteral("colorSchemePopup")) editor = window;
    QVERIFY(editor && !editor->isVisible());
    auto* mode=editor->findChild<QComboBox*>();auto* count=editor->findChild<QSpinBox*>();
    QVERIFY(mode&&count);QCOMPARE(mode->currentData().toInt(),static_cast<int>(okuflow::color_schemes::SchemeMode::Duotone));QCOMPARE(count->value(),2);
}
QTEST_MAIN(AgentControlPanelTests)
#include "agent_control_panel_tests.moc"

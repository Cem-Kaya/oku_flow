// Opt-in offscreen production-widget capture; no runtime, device or provider.
#include "okuflow/app/app.hpp"
#include "okuflow/common/codex_app_server_client.hpp"
#include "okuflow/ui/ai_settings_dialog.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/render_widget.hpp"
#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/ui/color_scheme_picker.hpp"
#include <QSlider>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDateTime>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QTranslator>
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/theme_contrast.hpp"
#include <cstdio>
namespace okuflow {
bool OkuFlowApp::HandlePanKey(int,bool) { qFatal("Unexpected app interaction"); return false; }
bool OkuFlowApp::HandlePanScroll(const QWheelEvent*) { qFatal("Unexpected app interaction"); return false; }
void OkuFlowApp::HandleZoomWheel(const QWheelEvent*) { qFatal("Unexpected app interaction"); }
void OkuFlowApp::HandleKeyboardZoom(float) { qFatal("Unexpected app interaction"); }
void OkuFlowApp::BeginMousePan(const QPointF&,const QSize&) { qFatal("Unexpected app interaction"); }
bool OkuFlowApp::UpdateMousePan(const QPointF&) { qFatal("Unexpected app interaction"); return false; }
void OkuFlowApp::EndMousePan() { qFatal("Unexpected app interaction"); }
QString CodexAppServerClient::BuiltInAssistantInstructions() { return QStringLiteral("Built-in instructions (fixture; no provider connected)."); }
}
namespace {
bool SaveWindow(okuflow::MainWindow& window,const QString& file)
{
    const qreal scale=window.devicePixelRatioF();
    QImage image(QSize(qRound(window.width()*scale),qRound(window.height()*scale)),QImage::Format_ARGB32_Premultiplied);image.setDevicePixelRatio(scale);image.fill(Qt::transparent);
    QPainter painter(&image);window.render(&painter);
    for(QWidget* peer:window.findChildren<QWidget*>())if(peer->isWindow()&&peer->isVisible()) {
        const QPoint offset=peer->mapToGlobal(QPoint())-window.mapToGlobal(QPoint());painter.drawPixmap(offset,peer->grab());
    }
    painter.end();return image.save(file);
}
}
int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);
    if(qEnvironmentVariableIntValue("OKUFLOW_UI_CAPTURE_DARK")==1)app.setPalette(okuflow::theme_contrast::PinnedDarkPalette(app.palette()));
    const int fontId=QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));if(fontId<0)return 7;
    QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).value(0),9));
    if(app.arguments().size()!=3||app.arguments().at(1)!="--output")return 2;
    const QString output=QDir::cleanPath(app.arguments().at(2));
    const QString local=QDir::cleanPath(qEnvironmentVariable("LOCALAPPDATA"));
    if(!QDir::isAbsolutePath(output)||!output.startsWith(local+QLatin1Char('/'),Qt::CaseInsensitive))return 3;
    QDir directory(output);if(!directory.mkpath(QStringLiteral(".")))return 4;
    okuflow::MainWindow window;window.resize(1672,941);window.setLocale(QLocale(QLocale::English,QLocale::UnitedStates));
    auto* contrast=window.contrastSlider();auto* readout=contrast->parentWidget()->findChild<QLabel*>(QStringLiteral("sliderValueLabel"));
    if(!readout)return 10;
    const int originalContrast=contrast->value();const QString originalText=readout->text();
    QSignalSpy changed(contrast,&QSlider::valueChanged);
    {const QSignalBlocker block(contrast);contrast->setValue(200);}
    if(readout->text()!=originalText)return 11;
    window.RefreshControlReadouts();if(!readout->text().contains(QStringLiteral("200"))||!changed.isEmpty())return 12;
    {const QSignalBlocker block(contrast);contrast->setValue(originalContrast);}window.RefreshControlReadouts();
    if(readout->text()!=originalText)return 13;
    window.setCameraPlaceholder(QStringLiteral("Waiting for camera"),QStringLiteral("Development fixture: no camera or provider is connected."));
    window.advancedModeButton()->setChecked(true);
    window.findChild<QTabWidget*>(QStringLiteral("advancedPage"))->setCurrentIndex(1);
    auto* panel=window.agentControlPanel();auto* view=window.assistantChatView();
    view->connectionLabel()->setText(QStringLiteral("ChatGPT · Connected (fixture)"));view->SetConnectionState(true);
    const auto message=[](const char* id,const char* role,const QString& body,const QJsonArray& actions=QJsonArray{}) {
        return QJsonObject{{"id",id},{"role",role},{"text",body},{"status","completed"},{"complete",true},{"origin",QString::fromLatin1(id).endsWith(QLatin1Char('1'))?QStringLiteral("explain"):QString{}},{"updatedAt",QDateTime(QDate(2026,10,14),QTime(10,24)).toMSecsSinceEpoch()},{"actions",actions}};
    };
    const QJsonObject chatSnapshot{{"conversationId","development-fixture"},{"kind","explain"},{"draft",""},{"busy",false},{"attachCurrentView",false},
        {"messages",QJsonArray{message("u1","user",QStringLiteral("I can't read the equation.")),
            message("a1","assistant",QStringLiteral("It is kinetic energy: K = ½mv².\nWould you like a closer view?")),
            message("u2","user",QStringLiteral("Zoom in a little.")),
            message("a2","assistant",QStringLiteral("Zoom is now 2×."),QJsonArray{QStringLiteral("View updated: Zoom 1× → 2×")}),
            message("u3","user",QStringLiteral("Yes, go ahead"))}}};
    view->SetChatSnapshot(chatSnapshot);
    panel->SetEnabled(true,false);panel->SetPendingApproval({{"approvalId","fixture-approval"},{"tool","capture.photo.take"},
        {"kind","action"},{"targetIdentity","fixture-current-view"},{"arguments",QJsonObject{}},{"expiresInMs",300000}});
    if(auto* split=window.findChild<QSplitter*>(QStringLiteral("advancedContentSplitter")))split->setSizes({1118,550});
    window.show();QTimer::singleShot(150,&app,[&] {
        bool saved=SaveWindow(window,directory.filePath(QStringLiteral("01-chat-first-assistant.png")));
        saved &= panel->approvalWidget()->grab().save(directory.filePath(QStringLiteral("02-photo-approval.png")));
        view->ShowSettings();QApplication::processEvents();saved &= SaveWindow(window,directory.filePath(QStringLiteral("03-settings-drawer.png")));view->CloseDrawer();
        okuflow::settings::AssistiveSettings settings;settings.codexExecutablePath.clear();settings.vlmApiKey.clear();
        okuflow::AiSettingsDialog dialog(settings,&window);dialog.resize(820,900);dialog.show();
        if(auto* scroll=dialog.findChild<QScrollArea*>())scroll->ensureWidgetVisible(dialog.findChild<QCheckBox*>(QStringLiteral("agentControlEnabled")),0,170);
        QApplication::processEvents();saved &= dialog.grab().save(directory.filePath(QStringLiteral("04-ai-settings-masters.png")));
        dialog.hide();
        settings.codexCodingEnabled=true;settings.codexInternetEnabled=true;
        settings.agentControlEnabled=true;okuflow::AiSettingsDialog exclusive(settings,&window);
        if(exclusive.result().codexCodingEnabled||exclusive.result().codexInternetEnabled)return app.exit(9);
        okuflow::AssistantChatView narrow;
        auto* narrowPanel=new okuflow::AgentControlPanel;
        narrow.SetSettingsWidget(narrowPanel);narrow.SetApprovalWidget(narrowPanel->approvalWidget());
        narrow.SetRoutingWidget(narrowPanel->routingNoticeWidget());narrow.SetChatSnapshot(chatSnapshot);
        narrowPanel->SetPendingApproval({{"approvalId","rtl-fixture"},{"tool","capture.photo.take"},{"arguments",QJsonObject{}},{"expiresInMs",300000}});
        narrow.resize(360,800);narrow.setLayoutDirection(Qt::RightToLeft);narrow.setLocale(QLocale(QStringLiteral("ar")));
        QTranslator arabic;if(!arabic.load(QStringLiteral(":/i18n/okuflow_ar.qm")))return app.exit(8);
        app.installTranslator(&arabic);okuflow::RetranslateWidgetTree(&narrow);narrow.show();QApplication::processEvents();
        saved &= narrow.grab().save(directory.filePath(QStringLiteral("05-narrow-arabic-rtl.png")));
        app.removeTranslator(&arabic);narrow.hide();
        okuflow::AssistiveOverlay floating(window.renderWidget());floating.setLocale(window.locale());
        floating.SetChatSnapshot(chatSnapshot);floating.chatView()->connectionLabel()->setText(QStringLiteral("ChatGPT · Connected (fixture)"));floating.chatView()->SetConnectionState(true);
        floating.agentControlPanel()->SetPendingApproval({{"approvalId","floating-fixture"},{"tool","capture.photo.take"},{"arguments",QJsonObject{}},{"expiresInMs",300000}});
        floating.ShowChat(true);floating.resize(550,820);QApplication::processEvents();
        saved &= floating.grab().save(directory.filePath(QStringLiteral("06-floating-explain-chat.png")));
        std::printf("%s\n",saved?"PASS: offscreen production widget captures":"FAIL: capture save");app.exit(saved?0:6);
    });return app.exec();
}

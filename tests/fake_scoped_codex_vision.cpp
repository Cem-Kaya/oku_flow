// Reuses the ordinary fake protocol, adding only private launch-profile proof.
#define main OrdinaryFixtureMain
#include "fake_codex_app_server.cpp"
#undef main
#include <QThread>
int main(int argc,char** argv) {
    QStringList args;for(int i=1;i<argc;++i)args.append(QString::fromLocal8Bit(argv[i]));
    const auto mode=qEnvironmentVariable("OKUFLOW_FAKE_SCENE_PROFILE_MODE");
    if(args.value(0)=="--version") {
        if(mode=="hold-version")QThread::msleep(30000);
        puts(mode=="bad-version"?"codex-cli 0.159.0":"codex-cli 0.160.1");return 0;
    }
    if(args.value(0)=="features") {
        if(mode=="bad-features") {puts("invented feature true");return 0;}
        QFile profile(QStringLiteral(OKUFLOW_AGENT_PROFILE_FIXTURE));if(!profile.open(QIODevice::ReadOnly))return 1;
        for(const auto& row:QJsonDocument::fromJson(profile.readAll()).object().value("featureInventory").toArray()) {
            const auto parts=row.toString().split('|');const auto text=(parts[0]+" "+parts[1]+" true\n").toUtf8();
            fwrite(text.data(),1,text.size(),stdout);
        }
        return 0;
    }
    if(args.value(0)=="mcp") {
        puts(mode=="bad-mcp"?"[{\"name\":\"bad.dot\"}]":"[{\"name\":\"fixture-private\",\"authorization\":\"fixture-secret\"}]");return 0;
    }
    if(qEnvironmentVariable("OKUFLOW_FAKE_SCENE_EXPECT_RESTRICTED")=="1") {
        // Both ordinary vision and scoped scene children require confinement.
        // A bare legacy launch must not produce isolation proof.
        if(args==QStringList{"app-server","--listen","stdio://"})return OrdinaryFixtureMain(argc,argv);
        for(const auto& pair:{QStringList{"--disable","apps"},QStringList{"--disable","plugins"},QStringList{"--disable","shell_tool"},
            QStringList{"-c","web_search=\"disabled\""},QStringList{"-c","mcp_servers.fixture-private.enabled=false"}}) {
            bool found=false;for(int i=0;i+1<args.size();++i)if(args[i]==pair[0] && args[i+1]==pair[1])found=true;
            if(!found)return 2;
        }
    }
    const auto path=qEnvironmentVariable("OKUFLOW_FAKE_SCENE_ARGS_FILE");
    if(!path.isEmpty()) {QFile proof(path);if(!proof.open(QIODevice::WriteOnly))return 1;
        proof.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact));}
    return OrdinaryFixtureMain(argc,argv);
}

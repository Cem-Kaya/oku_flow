// Private deterministic peer: no account, network, filesystem tools or camera.
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
#include <iostream>
static QJsonArray ScopeActions(const QString& scenario) {
    QJsonArray actions;
    auto add=[&](const char* tool,QJsonObject args={}) {
        actions.append(QJsonObject{{"tool",QString("okuflow_")+tool},{"arguments",args}});
    };
    if(scenario=="scope_center")add("view_center_set",{{"point",QJsonObject{{"space","scene"},{"x",0.25},{"y",0.75}}}});
    else if(scenario=="scope_reset")add("view_reset");
    else if(scenario=="scope_mode") {add("mode_list");add("mode_activate",{{"presetId","preset-reading"}});}
    else if(scenario=="scope_uct") {
        add("image_settings_get",{{"fields",QJsonArray{"image.uct.enabled","image.textClarity.enabled","image.uct.showOriginal"}}});
        add("image_settings_set",{{"image.textClarity.enabled",true},{"image.uct.enabled",true}});
        add("image_settings_get",{{"fields",QJsonArray{"image.uct.enabled","image.textClarity.enabled","image.uct.showOriginal"}}});
    }
    else if(scenario.startsWith("scope_colors_")) {
        auto intent=scenario.mid(QString("scope_colors_").size());
        QJsonObject args{{"intent",intent}};if(intent=="scheme")args["schemeId"]="yellow-black";
        add("display_reading_colors_set",args);
    } else if(scenario=="scope_contrast")add("image_contrast_set",{{"contrast",1.237},{"brightness",0.127}});
    else if(scenario=="scope_auto")add("image_auto_adjust",{{"goal","board"},{"allowModeChange",false}});
    else if(scenario.startsWith("scope_orientation")) {
        add("camera_orientation_set",{{"degreesClockwise",90}});
        if(scenario=="scope_orientation_denied")add("camera_orientation_set",{{"degreesClockwise",90}});
    } else if(scenario=="scope_observe")add("view_observe");
    else if(scenario=="scope_photo") {
        add("capture_photo_take");add("app_operation_get",{{"operationId","$operation"}});
        add("app_wait_for",{{"condition","operation_state"},{"operationId","$operation"},{"timeoutMs",0}});
        add("app_operation_get",{{"operationId","$operation"}});
    } else if(scenario=="scope_recording") {
        add("capture_recording_start",{{"audio","none"}});
        add("app_wait_for",{{"condition","operation_state"},{"operationId","$operation"},{"timeoutMs",0}});
        add("app_operation_get",{{"operationId","$operation"}});
        add("capture_recording_stop",{{"sessionId","$session"}});add("app_operation_get",{{"operationId","$operation"}});
        add("app_wait_for",{{"condition","operation_state"},{"operationId","$operation"},{"timeoutMs",0}});
        add("app_operation_get",{{"operationId","$operation"}});
    } else if(scenario=="scope_recording_start")add("capture_recording_start",{{"audio","none"}});
    else if(scenario=="scope_manual_stop")add("capture_recording_stop",{{"sessionId","fixture-manual"}});
    else if(scenario=="scope_stale_stop")add("capture_recording_stop",{{"sessionId","fixture-retired"}});
    else if(scenario=="scope_camera") {add("camera_sources_list");add("camera_select",{{"cameraHandle","cam-0123456789abcdef0123456789abcdef"}});}
    else if(scenario=="scope_user_wins")add("image_contrast_set",{{"contrast",2},{"brightness",0}});
    return actions;
}
static void Send(QJsonObject value) {
    auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
    fwrite(bytes.data(),1,bytes.size(),stdout);fputc('\n',stdout);fflush(stdout);
}
int main(int argc,char** argv) {
    const QString first=argc>1?QString::fromLocal8Bit(argv[1]):QString{};
    if(first=="--version") {puts("codex-cli 0.160.1");return 0;}
    if(first=="features") {
        QFile profile(QStringLiteral(OKUFLOW_AGENT_PROFILE_FIXTURE));if(!profile.open(QIODevice::ReadOnly))return 1;
        for(auto row:QJsonDocument::fromJson(profile.readAll()).object()["featureInventory"].toArray()) {
            const auto parts=row.toString().split('|');auto text=(parts[0]+" "+parts[1]+" false\n").toUtf8();
            fwrite(text.data(),1,text.size(),stdout);
        }return 0;
    }
    if(first=="mcp") {puts("[]");return 0;}
    const auto scenario=qEnvironmentVariable("OKUFLOW_FAKE_AGENT_SCENARIO");
    bool probe=false;QString thread,turn;int threads=0,turns=0;
    const auto scope=ScopeActions(scenario);int nextAction=0;
    QJsonArray replies;QString operation,session;
    for(std::string line;std::getline(std::cin,line);) {
        const auto msg=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto method=msg["method"].toString();const auto params=msg["params"].toObject();
        const auto id=msg["id"];
        auto result=[&](QJsonObject value){Send({{"id",id},{"result",value}});};
        auto notify=[&](QString method,QJsonObject p){p["threadId"]=thread;p["turnId"]=turn;Send({{"method",method},{"params",p}});};
        auto call=[&](QString turnId,QString name,QString callId,QJsonObject args){Send({{"id",callId},
            {"method","item/tool/call"},{"params",QJsonObject{{"threadId",thread},{"turnId",turnId},
                {"callId",callId},{"tool",name},{"arguments",args}}}});};
        auto scopeCall=[&] {
            const auto step=scope[nextAction].toObject();auto args=step["arguments"].toObject();
            for(auto it=args.begin();it!=args.end();++it) {
                if(it.value()=="$operation")it.value()=operation;
                if(it.value()=="$session")it.value()=session;
            }
            call(turn,step["tool"].toString(),QString("scope-%1").arg(nextAction++),args);
        };
        if(method=="initialize") {
            if(!params["capabilities"].toObject()["experimentalApi"].toBool())return 3;
            result({});
        } else if(method=="thread/start") {
            const auto tools=params["dynamicTools"].toArray();
            probe=tools.size()==1 && tools[0].toObject()["name"]=="okuflow_ping";
            if(!probe && scenario=="scope_uct") {
                const auto policy=params["developerInstructions"].toString();
                if(!policy.contains("Ultra Clear Text") || !policy.contains("image.uct.enabled") ||
                   !policy.contains("image.textClarity.enabled"))return 44;
                bool discovered=false;
                for(const auto tool:tools)if(tool.toObject()["name"]=="okuflow_image_settings_set") {
                    const auto descriptor=tool.toObject();
                    discovered=descriptor["description"].toString().contains("Ultra Clear Text") &&
                        descriptor["description"].toString().contains("OCR") &&
                        descriptor["inputSchema"].toObject()["properties"].toObject().contains("image.uct.enabled");
                }
                if(!discovered)return 45;
            }
            if(!probe)for(const auto step:scope) {
                bool advertised=false;
                for(const auto tool:tools)advertised|=tool.toObject()["name"]==step.toObject()["tool"];
                if(!advertised)return 43; // a scripted call must be declared by this adapter
            }
            thread=QString("thread_%1").arg(++threads);result({{"thread",QJsonObject{{"id",thread},{"model",params["model"]}}}});
        } else if(method=="turn/start") {
            if(params["threadId"].toString()!=thread || thread.isEmpty())return 42;
            turn=probe?"turn_probe":QString("turn_%1").arg(++turns);
            if(probe && scenario=="probe_no_call") {
                result({{"turn",QJsonObject{{"id",turn}}}});
                notify("turn/completed",{{"turn",QJsonObject{{"id",turn},{"status","completed"}}}});
                continue;
            }
            if(probe && scenario=="probe_start_rejected") {
                Send({{"id",id},{"error",QJsonObject{{"code",-32602},{"message","private-provider-detail"}}}});
                continue;
            }
            if(!probe && scenario=="stale_first")call("orphan_old_turn","okuflow_view_zoom_set","stale",{{"magnification",7}});
            if(!probe && scenario=="retired_first")call("turn_probe","okuflow_view_zoom_set","stale",{{"magnification",7}});
            if(!probe && scenario=="forbidden_item")notify("item/started",{{"item",QJsonObject{{"type","commandExecution"}}}});
            // Real tool call precedes authoritative start reply. No action may
            // execute until the correlated reply binds turn id.
            if(!probe && !scope.isEmpty()) {nextAction=0;replies={};operation.clear();session.clear();scopeCall();}
            else call(turn,probe?"okuflow_ping":scenario=="forbidden_tool"?"shell":"okuflow_view_zoom_set",
                probe?"ping":"action",probe?QJsonObject{}:QJsonObject{{"magnification",2}});
            result({{"turn",QJsonObject{{"id",turn}}}});
        } else if(method=="turn/interrupt")result({});
        else if(method.isEmpty() && msg.contains("result") && id.toString().startsWith("scope-")) {
            const auto response=msg["result"].toObject();QJsonObject data;int images=0;
            for(const auto item:response["contentItems"].toArray()) {
                const auto content=item.toObject();
                if(content["type"]=="inputText")data=QJsonDocument::fromJson(content["text"].toString().toUtf8()).object();
                if(content["type"]=="inputImage")++images;
            }
            if(data.contains("operationId"))operation=data["operationId"].toString();
            if(data.contains("sessionId"))session=data["sessionId"].toString();
            replies.append(QJsonObject{{"success",response["success"]},{"data",data},{"images",images}});
            if(nextAction<scope.size())scopeCall();
            else {
                notify("item/agentMessage/delta",{{"delta",QString::fromUtf8(QJsonDocument(QJsonObject{{"replies",replies}}).toJson(QJsonDocument::Compact))}});
                notify("turn/completed",{{"turn",QJsonObject{{"id",turn},{"status","completed"}}}});
            }
        } else if(method.isEmpty() && msg.contains("result") && (id=="ping" || id=="action")) {
            if(!msg["result"].toObject()["success"].toBool())continue;
            notify("item/agentMessage/delta",{{"delta",probe?"ready":"done"}});
            notify("turn/completed",{{"turn",QJsonObject{{"id",turn},{"status","completed"}}}});
        }
    }
    return 0;
}

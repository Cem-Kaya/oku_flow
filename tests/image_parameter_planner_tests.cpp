#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/constants.hpp"
#include <QtTest>
#include <QJsonArray>
#include <set>

using namespace okuflow::agent;
namespace {
std::map<Field,QJsonValue> Defaults() {
    std::map<Field,QJsonValue> fields;
    for(const auto& descriptor:ParameterRegistry()) {
        if(descriptor.group!=ParameterGroup::Image && descriptor.group!=ParameterGroup::View)continue;
        const auto field=AppControlTarget::ImageParameterField(descriptor.id);if(!field)continue;
        const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(std::string(descriptor.schema))).object();
        const auto type=schema["type"].toString();
        if(type=="boolean")fields[*field]=false;
        else if(schema.contains("enum"))fields[*field]=schema["enum"].toArray().first();
        else if(type=="number" || type=="integer")fields[*field]=schema["minimum"].toDouble();
        else if(type=="array")fields[*field]=QJsonArray{"#000000","#ffffff"};
    }
    fields[Field::ZoomAmount]=1.0;fields[Field::CenterX]=0.5;fields[Field::CenterY]=0.5;
    fields[Field::Threshold]=0.5;fields[Field::BlurSigma]=1.0;fields[Field::BlurRadius]=3;
    fields[Field::TemporalAlpha]=double(0.25f);fields[Field::Contrast]=1.0;fields[Field::Brightness]=0.0;
    fields[Field::SuperResStrength]=double(0.65f);fields[Field::Upscaler]=0;fields[Field::Polarity]=0;
    fields[Field::CustomColorStopCount]=2;
    fields[Field::CustomColorRecipe]=QJsonObject{{"mode","duotone"},{"stopCount",2},
        {"stops",QJsonArray{"#000000","#ffffff"}},{"stepped",false},{"schemeId","normal"}};
    return fields;
}
QJsonValue Written(const ControlPlan& plan,Field field) {
    for(const auto& write:plan.writes)if(write.field==field)return write.value;
    return QJsonValue(QJsonValue::Undefined);
}
ControlPlan Image(const std::map<Field,QJsonValue>& fields,const QJsonObject& args) {
    return AppControlTarget::PlanImageParameterDependencies(fields,ToolId::ImageSettingsSet,args);
}
QJsonObject Custom(const char* mode="gradient",bool stepped=false) {
    return {{"image.colors.custom.mode",mode},{"image.colors.custom.stopCount",2},
        {"image.colors.custom.stops",QJsonArray{"#ABCDEF","#123456"}},{"image.colors.custom.stepped",stepped}};
}
}

class ImageParameterPlannerTests final:public QObject {
    Q_OBJECT
private slots:
    void everyImageViewDescriptorHasAnIndependentField() {
        int image=0,view=0;std::set<Field> fields;
        for(const auto& descriptor:ParameterRegistry()) {
            if(descriptor.group!=ParameterGroup::Image && descriptor.group!=ParameterGroup::View)continue;
            const auto field=AppControlTarget::ImageParameterField(descriptor.id);QVERIFY(field);
            QVERIFY(fields.insert(*field).second);
            descriptor.group==ParameterGroup::Image?++image:++view;
            const auto tool=descriptor.group==ParameterGroup::Image?ToolId::ImageSettingsGet:ToolId::ViewSettingsGet;
            const auto plan=AppControlTarget::PlanImageParameterDependencies(Defaults(),tool,
                {{"fields",QJsonArray{QString::fromUtf8(descriptor.id.data(),qsizetype(descriptor.id.size()))}}});
            QVERIFY(plan.error.isEmpty());QCOMPARE(plan.observedFields.size(),size_t(1));QCOMPARE(plan.observedFields.front(),*field);
        }
        QCOMPARE(image,49);QCOMPARE(view,9);QVERIFY(!AppControlTarget::ImageParameterField("image.arbitrary.setting"));
    }
    void invalidSecondFieldRejectsTheWholePlan() {
        const auto fields=Defaults();
        const auto plan=Image(fields,{{"image.contrast",2.0},{"image.blur.sigma",99.0}});
        QCOMPARE(plan.error,"invalid_argument");QVERIFY(plan.writes.empty());QVERIFY(fields==Defaults());
    }
    void wrongTypeUnknownKeyAndMixedGroupHaveNoWrites() {
        for(const auto args:{QJsonObject{{"image.blur.enabled","true"}},QJsonObject{{"image.contrast",2.0},{"unknown",true}},
            QJsonObject{{"image.contrast",2.0},{"view.zoom.enabled",true}}}) {
            const auto plan=Image(Defaults(),args);QCOMPARE(plan.error,"invalid_argument");QVERIFY(plan.writes.empty());
        }
    }
    void blurNormalizationMatchesNativeSliderPolicy() {
        const auto plan=Image(Defaults(),{{"image.blur.sigma",1.26},{"image.blur.radius",15}});
        QVERIFY(plan.error.isEmpty());QCOMPARE(Written(plan,Field::BlurSigma).toDouble(),double(okuflow::app_constants::SliderValueToSigma(13)));
        QCOMPARE(Written(plan,Field::BlurRadius).toInt(),15);
        const auto invalid=Image(Defaults(),{{"image.blur.radius",13}});
        QCOMPARE(invalid.error,"invalid_argument");QVERIFY(invalid.writes.empty());
        QCOMPARE(okuflow::app_constants::SnapBlurRadius(13),15); // native drafts snap; wire enum is already closed
    }
    void enablingSuperResIncludesZoomAndRestoredStrength() {
        auto fields=Defaults();fields[Field::SuperResStrength]=0.0;
        const auto plan=Image(fields,{{"image.superResolution.enabled",true}});
        QVERIFY(plan.error.isEmpty());QVERIFY(Written(plan,Field::ZoomEnabled).toBool());
        QCOMPARE(Written(plan,Field::ZoomAmount).toDouble(),double(1.33f));
        QCOMPARE(Written(plan,Field::SuperResStrength).toDouble(),double(0.65f));
        QCOMPARE(plan.writes.size(),size_t(4));QVERIFY(plan.requiresApproval);QCOMPARE(plan.approvalDomain,ApprovalDomain::Configuration);
    }
    void fasterTwoXExcludesUltraAndIncludesTwoXMinimum() {
        auto fields=Defaults();fields[Field::SuperResEnabled]=true;fields[Field::SuperResUltra1440p]=true;
        const auto plan=Image(fields,{{"image.superResolution.prefer2x",true}});
        QVERIFY(plan.error.isEmpty());QVERIFY(!Written(plan,Field::SuperResUltra1440p).toBool());
        QCOMPARE(Written(plan,Field::ZoomAmount).toDouble(),2.0);QVERIFY(Written(plan,Field::ZoomEnabled).toBool());
        QCOMPARE(plan.writes.size(),size_t(4));
    }
    void ultraExcludesTwoXWithoutInventingZoomChange() {
        auto fields=Defaults();fields[Field::SuperResEnabled]=true;fields[Field::SuperResPrefer2x]=true;fields[Field::ZoomAmount]=2.0;
        const auto plan=Image(fields,{{"image.superResolution.ultra1440p",true}});
        QVERIFY(plan.error.isEmpty());QVERIFY(!Written(plan,Field::SuperResPrefer2x).toBool());
        QVERIFY(Written(plan,Field::ZoomAmount).isUndefined());QCOMPARE(plan.writes.size(),size_t(2));
        const auto invalid=Image(fields,{{"image.superResolution.ultra1440p",true},{"image.superResolution.prefer2x",true}});
        QCOMPARE(invalid.error,"invalid_argument");QVERIFY(invalid.writes.empty());
    }
    void disablingSuperResKeepsRequestedZeroStrengthAndClearsOverride() {
        auto fields=Defaults();fields[Field::SuperResEnabled]=true;fields[Field::SuperResPerformanceOverride]=true;
        const auto plan=Image(fields,{{"image.superResolution.enabled",false},{"image.superResolution.strength",0.0}});
        QVERIFY(plan.error.isEmpty());QCOMPARE(Written(plan,Field::SuperResStrength).toDouble(),0.0);
        QVERIFY(!Written(plan,Field::SuperResPerformanceOverride).toBool());QCOMPARE(plan.writes.size(),size_t(3));
        QVERIFY(!plan.requiresApproval);
    }
    void explicitViewZoomCannotBeSilentlyInvertedUnderSuperRes() {
        auto fields=Defaults();fields[Field::SuperResEnabled]=true;fields[Field::SuperResPrefer2x]=true;
        const auto plan=AppControlTarget::PlanImageParameterDependencies(fields,ToolId::ViewSettingsSet,
            {{"view.zoom.amount",1.0},{"view.zoom.enabled",false}});
        QCOMPARE(plan.error,"invalid_argument");QVERIFY(plan.writes.empty());
        for(const auto request:{QJsonObject{{"view.zoom.enabled",false}},QJsonObject{{"view.zoom.amount",1.0}}}) {
            const auto bad=AppControlTarget::PlanImageParameterDependencies(fields,ToolId::ViewSettingsSet,request);
            QCOMPARE(bad.error,"invalid_argument");QVERIFY(bad.writes.empty());
        }
        fields[Field::ZoomAmount]=3.0;fields[Field::ZoomEnabled]=true;
        const auto valid=AppControlTarget::PlanImageParameterDependencies(fields,ToolId::ViewSettingsSet,{{"view.zoom.amount",2.5}});
        QVERIFY(valid.error.isEmpty());QCOMPARE(Written(valid,Field::ZoomAmount).toDouble(),2.5);
        QVERIFY(Written(valid,Field::ZoomEnabled).isUndefined());
    }
    void unchangedNativeHandlersHaveNoImplicitResets() {
        auto fields=Defaults();fields[Field::Stabilization]=true;fields[Field::Keystone]=true;fields[Field::KeystonePaused]=true;
        for(const auto request:{QJsonObject{{"image.stabilization.enabled",true}},QJsonObject{{"image.keystone.enabled",true}}}) {
            const auto plan=Image(fields,request);QVERIFY(plan.error.isEmpty());QVERIFY(Written(plan,Field::KeystonePaused).isUndefined());
            QCOMPARE(plan.writes.size(),size_t(1));
        }
        fields[Field::SuperResEnabled]=true;fields[Field::SuperResStrength]=0.0;fields[Field::ZoomEnabled]=false;
        for(const auto request:{QJsonObject{{"image.textClarity.enabled",false}},QJsonObject{{"image.superResolution.enabled",true}}}) {
            const auto plan=Image(fields,request);QVERIFY(plan.error.isEmpty());
            QVERIFY(Written(plan,Field::ZoomEnabled).isUndefined());QVERIFY(Written(plan,Field::ZoomAmount).isUndefined());
            QVERIFY(Written(plan,Field::SuperResStrength).isUndefined());QVERIFY(!plan.requiresApproval);
            QCOMPARE(plan.writes.size(),size_t(1));
        }
    }
    void disablingSuperResOnlyNeedsNoApprovalForItsImplicitOverrideReset() {
        auto fields=Defaults();fields[Field::SuperResEnabled]=true;fields[Field::SuperResPerformanceOverride]=true;
        const auto plan=Image(fields,{{"image.superResolution.enabled",false}});QVERIFY(plan.error.isEmpty());
        QVERIFY(!Written(plan,Field::SuperResEnabled).toBool());QVERIFY(!Written(plan,Field::SuperResPerformanceOverride).toBool());
        QCOMPARE(plan.writes.size(),size_t(2));QVERIFY(!plan.requiresApproval);QCOMPARE(plan.approvalDomain,ApprovalDomain::None);
    }
    void missingCoupledFieldRejectsWithoutPublishingPartialWrites() {
        auto fields=Defaults();fields.erase(Field::ZoomAmount);
        const auto plan=Image(fields,{{"image.superResolution.enabled",true}});
        QCOMPARE(plan.error,"unavailable");QVERIFY(plan.writes.empty());
    }
    void getterFiltersDoNotRefreshUnreturnedFields() {
        const auto one=AppControlTarget::PlanImageParameterDependencies(Defaults(),ToolId::ImageSettingsGet,
            {{"fields",QJsonArray{"image.contrast"}}});
        QVERIFY(one.error.isEmpty());QCOMPARE(one.observedFields,std::vector<Field>{Field::Contrast});QVERIFY(one.writes.empty());
        for(const auto request:{QJsonArray{},QJsonArray{"image.contrast","image.contrast"},QJsonArray{"view.zoom.enabled"}}) {
            const auto bad=AppControlTarget::PlanImageParameterDependencies(Defaults(),ToolId::ImageSettingsGet,{{"fields",request}});
            QCOMPARE(bad.error,"invalid_argument");QVERIFY(bad.writes.empty());QVERIFY(bad.observedFields.empty());
        }
    }
    void customRecipeIsCompleteAtomicAndNormalized() {
        const auto plan=Image(Defaults(),Custom());QVERIFY(plan.error.isEmpty());
        QCOMPARE(Written(plan,Field::Scheme).toString(),"custom");QCOMPARE(plan.writes.front().field,Field::CustomColorRecipe);
        QCOMPARE(Written(plan,Field::CustomColorStops).toArray().first().toString(),"#abcdef");
        QCOMPARE(plan.writes.size(),size_t(6));
        const auto poster=Image(Defaults(),Custom("posterize",false));QVERIFY(poster.error.isEmpty());QVERIFY(Written(poster,Field::CustomColorStepped).toBool());
        const auto incomplete=Image(Defaults(),{{"image.colors.custom.mode","gradient"}});
        QCOMPARE(incomplete.error,"invalid_argument");QVERIFY(incomplete.writes.empty());
        auto mismatch=Custom();mismatch["image.colors.custom.stopCount"]=3;
        const auto invalid=Image(Defaults(),mismatch);QCOMPARE(invalid.error,"invalid_argument");QVERIFY(invalid.writes.empty());
        auto badColor=Custom();badColor["image.colors.custom.stops"]=QJsonArray{"#zz0000","#ffffff"};
        const auto bad=Image(Defaults(),badColor);QCOMPARE(bad.error,"invalid_argument");QVERIFY(bad.writes.empty());
    }
    void invalidRecipeSelectionRejectsAnOtherwiseValidRefinementBatch() {
        const auto before=Defaults();auto request=Custom();
        request["image.textClarity.enabled"]=true;request["image.colors.schemeId"]="not-a-built-in-scheme";
        const auto invalid=Image(before,request);
        QCOMPARE(invalid.error,"invalid_argument");QVERIFY(invalid.writes.empty());QVERIFY(before==Defaults());
        request["image.colors.schemeId"]="normal";
        const auto valid=Image(before,request);QVERIFY(valid.error.isEmpty());
        QCOMPARE(Written(valid,Field::CustomColorRecipe).toObject()["schemeId"].toString(),QString("normal"));
        QVERIFY(Written(valid,Field::TextClarity).toBool());
    }
    void disablingStabilizationReportsBumpAndPauseCouplings() {
        auto fields=Defaults();fields[Field::Stabilization]=true;fields[Field::BumpHold]=true;fields[Field::KeystonePaused]=true;
        const auto plan=Image(fields,{{"image.stabilization.enabled",false}});QVERIFY(plan.error.isEmpty());
        QVERIFY(!Written(plan,Field::BumpHold).toBool());QVERIFY(!Written(plan,Field::KeystonePaused).toBool());QCOMPARE(plan.writes.size(),size_t(3));
        const auto bump=Image(Defaults(),{{"image.stabilization.bumpHold",true}});QCOMPARE(bump.error,"invalid_argument");QVERIFY(bump.writes.empty());
        const auto pause=Image(Defaults(),{{"image.keystone.paused",true}});QCOMPARE(pause.error,"invalid_argument");QVERIFY(pause.writes.empty());
    }
    void textChildMasterTruthTable_data() {
        QTest::addColumn<int>("field");QTest::addColumn<bool>("master");QTest::addColumn<bool>("child");
        for(const auto field:{Field::Flatten,Field::Adaptive,Field::SmartSharpen,Field::Clahe,Field::TwoColor,
            Field::Hysteresis,Field::SelectiveSharpen,Field::FocusDetection,Field::Glare})
            for(const bool master:{false,true})for(const bool child:{false,true})
                QTest::newRow(qPrintable(QString("%1-master%2-child%3").arg(int(field)).arg(master).arg(child)))<<int(field)<<master<<child;
    }
    void textChildMasterTruthTable() {
        QFETCH(int,field);QFETCH(bool,master);QFETCH(bool,child);
        auto fields=Defaults();fields[Field::TextClarity]=master;fields[static_cast<Field>(field)]=child;
        if(static_cast<Field>(field)==Field::TwoColor)fields[Field::Adaptive]=true;
        if(static_cast<Field>(field)==Field::SelectiveSharpen)fields[Field::SmartSharpen]=true;
        if(static_cast<Field>(field)==Field::Hysteresis)fields[Field::Adaptive]=true;
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(static_cast<Field>(field),fields),master && child);
    }
    void consumedMaskDependencies_data() {
        QTest::addColumn<bool>("master");QTest::addColumn<bool>("adaptive");QTest::addColumn<bool>("smart");
        QTest::addColumn<bool>("selective");QTest::addColumn<bool>("hysteresis");
        for(const bool master:{false,true})for(const bool adaptive:{false,true})for(const bool smart:{false,true})
            for(const bool selective:{false,true})for(const bool hysteresis:{false,true})
                QTest::newRow(qPrintable(QString("master%1-adaptive%2-smart%3-selective%4-hysteresis%5")
                    .arg(master).arg(adaptive).arg(smart).arg(selective).arg(hysteresis)))<<master<<adaptive<<smart<<selective<<hysteresis;
    }
    void consumedMaskDependencies() {
        QFETCH(bool,master);QFETCH(bool,adaptive);QFETCH(bool,smart);QFETCH(bool,selective);QFETCH(bool,hysteresis);
        auto fields=Defaults();fields[Field::TextClarity]=master;fields[Field::Adaptive]=adaptive;
        fields[Field::SmartSharpen]=smart;fields[Field::SelectiveSharpen]=selective;fields[Field::Hysteresis]=hysteresis;
        const bool activeAdaptive=master && adaptive;
        const bool activeSmart=master && smart && !activeAdaptive;
        const bool activeSelective=activeSmart && selective;
        const bool consumedMask=activeAdaptive || activeSelective;
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(Field::SelectiveSharpen,fields),activeSelective);
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(Field::SmartStrength,fields),activeSmart);
        for(const auto field:{Field::Hysteresis,Field::HysteresisStrength})
            QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(field,fields),consumedMask && hysteresis);
        for(const auto field:{Field::Sauvola,Field::Softness,Field::Polarity,Field::Stroke})
            QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(field,fields),consumedMask);
    }
    void adaptiveShadowsBlackWhiteOnlyWhenBothFlagsEnableIt_data() {
        QTest::addColumn<bool>("master");QTest::addColumn<bool>("adaptive");QTest::addColumn<bool>("twoColor");
        for(const bool master:{false,true})for(const bool adaptive:{false,true})for(const bool twoColor:{false,true})
            QTest::newRow(qPrintable(QString("master%1-adaptive%2-two%3").arg(master).arg(adaptive).arg(twoColor)))<<master<<adaptive<<twoColor;
    }
    void adaptiveShadowsBlackWhiteOnlyWhenBothFlagsEnableIt() {
        QFETCH(bool,master);QFETCH(bool,adaptive);QFETCH(bool,twoColor);
        auto fields=Defaults();fields[Field::TextClarity]=master;fields[Field::Adaptive]=adaptive;fields[Field::TwoColor]=twoColor;
        fields[Field::BlackWhite]=true;fields[Field::SmartSharpen]=true;
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(Field::BlackWhite,fields),!(master && adaptive));
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(Field::Threshold,fields),!(master && adaptive));
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(Field::Scheme,fields),!(master && adaptive && twoColor));
        QCOMPARE(AppControlTarget::ImageParameterEffectiveIntent(Field::SmartSharpen,fields),master && !adaptive);
    }
};
QTEST_GUILESS_MAIN(ImageParameterPlannerTests)
#include "image_parameter_planner_tests.moc"

#include <QtTest>
#include <QJsonDocument>
#include "okuflow/app/agent/assistant_parameter_policy.hpp"
using namespace okuflow::agent;
class AssistantParameterPolicyTests : public QObject {
    Q_OBJECT
private slots:
    void wholePatchRejectsProtectedUnknownAndCrossGroupFields() {
        okuflow::settings::AssistiveSettings cfg;
        for(const auto& field:{"assistant_preferences.apiKey","assistant_preferences.workspace","assistant_preferences.coding","unknown","notes_output.lectureNotes"}) {
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.overlayEnabled",true},{field,false}},false);
            QCOMPARE(plan.error,QString("invalid_argument"));QVERIFY(plan.writes.empty());
        }
        const auto notes=PrepareAssistantPreferencePatch(cfg,{}, {},{{"notes_output.lectureNotes",false}},true);
        QVERIFY(notes.error.isEmpty());QCOMPARE(notes.writes.size(),size_t(1));QCOMPARE(notes.writes[0].field,Field::LectureNotes);
        QVERIFY(!notes.requiresApproval);
    }
    void sensitiveConfigurationRequiresDistinctExactApproval() {
        okuflow::settings::AssistiveSettings cfg;
        for(const auto& patch:{QJsonObject{{"assistant_preferences.provider","openai-compatible"}},
            QJsonObject{{"assistant_preferences.instructions","Concise answers"}},
            QJsonObject{{"assistant_preferences.explainPrompt","Describe this image"}},
            QJsonObject{{"assistant_preferences.httpModel","fixture-model"}}}) {
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},patch,false);
            QVERIFY(plan.error.isEmpty());QVERIFY(plan.requiresApproval);QCOMPARE(plan.approvalDomain,ApprovalDomain::Configuration);
        }
        const auto scene=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.sceneExplain",true}},false);
        QVERIFY(scene.error.isEmpty());QVERIFY(!scene.requiresApproval);QCOMPARE(scene.approvalDomain,ApprovalDomain::None);
        QVERIFY(scene.requiresFrames);
    }
    void changedModelAndReasoningAreValidatedTogether() {
        okuflow::settings::AssistiveSettings cfg;cfg.codexModel="old";cfg.codexReasoningEffort="high";
        const QJsonArray models{QJsonObject{{"id","new"},{"supportedReasoningEfforts",QJsonArray{"low",QJsonObject{{"reasoningEffort","medium"}}}}}};
        const auto plan=PrepareAssistantPreferencePatch(cfg,models,{},{{"assistant_preferences.codexModel","new"}},false);
        QVERIFY(plan.error.isEmpty());QCOMPARE(plan.writes.size(),size_t(2));QCOMPARE(plan.writes[1].field,Field::AssistantReasoning);QCOMPARE(plan.writes[1].value,QJsonValue("low"));
        const auto invalid=PrepareAssistantPreferencePatch(cfg,models,{},{{"assistant_preferences.codexModel","new"},{"assistant_preferences.codexReasoning","high"}},false);
        QCOMPARE(invalid.error,QString("unavailable"));QVERIFY(invalid.writes.empty());
        const auto unknown=PrepareAssistantPreferencePatch(cfg,models,{},{{"assistant_preferences.codexModel","invented"}},false);
        QCOMPARE(unknown.error,QString("unavailable"));QVERIFY(unknown.writes.empty());
        const auto old=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.codexModel","old"},{"assistant_preferences.codexReasoning","high"}},false);
        QVERIFY(old.error.isEmpty()); // Existing legacy preference is not silently rewritten by missing catalog.
    }
    void secretsControlsAndBoundsCannotEnterPreferenceWrites() {
        okuflow::settings::AssistiveSettings cfg;cfg.vlmApiKey="private-token";
        for(const auto& text:{QString("private-token"),QString("environment-token"),QString(4097,'x'),QString("bad\x01text")}) {
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.instructions",text},{"assistant_preferences.overlayEnabled",true}},false,"environment-token");
            QCOMPARE(plan.error,QString("invalid_argument"));QVERIFY(plan.writes.empty());
        }
        const auto rate=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.ttsRate",1.01}},false);
        QCOMPARE(rate.error,QString("invalid_argument"));QVERIFY(rate.writes.empty());
        const auto lines=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.instructions","first\nsecond"}},false);
        QVERIFY(lines.error.isEmpty());
    }
    void voicesUseOnlyInstalledOpaqueHandles() {
        okuflow::settings::AssistiveSettings cfg;
        const QString handle="voice-0123456789abcdef0123456789abcdef";
        const auto ok=PrepareAssistantPreferencePatch(cfg,{}, {handle},{{"assistant_preferences.ttsVoice",handle}},false);
        QVERIFY(ok.error.isEmpty());
        const auto missing=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.ttsVoice",handle}},false);
        QCOMPARE(missing.error,QString("unavailable"));QVERIFY(missing.writes.empty());
        const auto path=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.ttsVoice","C:/voice.wav"}},false);
        QCOMPARE(path.error,QString("unavailable"));QVERIFY(path.writes.empty());
    }
    void finiteLengthsCountUnicodeCodepoints() {
        okuflow::settings::AssistiveSettings cfg;
        const auto emoji=QString::fromUcs4(U"\U0001F642").repeated(4096);
        QCOMPARE(emoji.size(),qsizetype(8192));
        const auto valid=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.instructions",emoji}},false);
        QVERIFY(valid.error.isEmpty()); // Separate transport byte budget still applies at core/provider admission.
        const auto invalid=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.instructions",emoji+QString::fromUcs4(U"\U0001F642")}},false);
        QCOMPARE(invalid.error,QString("invalid_argument"));QVERIFY(invalid.writes.empty());
    }
    void fullFiniteValuesAndTypedEqualityStayWithinResultBudget() {
        const auto text=QString::fromUcs4(U"\U0001F642").repeated(4096);
        const QJsonObject row{{"requested",text},{"effective",text},{"saved",text},{"available",true},{"unsupportedReason",""}};
        const auto result=BoundAssistantPreferenceRead({{"assistant_preferences.instructions",row}});
        const auto value=result.value("parameters").toObject().value("assistant_preferences.instructions").toObject();
        QCOMPARE(value.value("requested").toString(),text);
        QCOMPARE(value.value("effective").toObject().value("sameAs").toString(),QString("requested"));
        QCOMPARE(value.value("saved").toObject().value("sameAs").toString(),QString("requested"));
        QVERIFY(!value.contains("filterRequired"));
        QVERIFY(QJsonDocument(result).toJson(QJsonDocument::Compact).size()<=29*1024);
        const auto ordinary=QString(2000,'x');
        const auto shortValue=BoundAssistantPreferenceRead({{"assistant_preferences.instructions",QJsonObject{{"requested",ordinary},{"effective",ordinary},{"saved",ordinary}}}})
            .value("parameters").toObject().value("assistant_preferences.instructions").toObject();
        QCOMPARE(shortValue.value("requested").toString(),ordinary);QCOMPARE(shortValue.value("saved").toString(),ordinary);
        QVERIFY(!shortValue.contains("filterRequired"));
    }
    void distinctSavedAndRequestedValuesUseExplicitProjection() {
        const auto requested=QString::fromUcs4(U"\U0001F642").repeated(4096);
        const auto saved=QString::fromUcs4(U"\U0001F643").repeated(4096);
        const QJsonObject parameters{{"assistant_preferences.instructions",QJsonObject{{"requested",requested},{"effective",requested},{"saved",saved},{"available",true}}}};
        const auto all=BoundAssistantPreferenceRead(parameters).value("parameters").toObject().value("assistant_preferences.instructions").toObject();
        QVERIFY(all.value("requested").isNull());QVERIFY(all.value("saved").isNull());
        QCOMPARE(all.value("unsupportedReason").toString(),QString("response_budget"));QVERIFY(all.value("filterRequired").toBool());
        const auto current=BoundAssistantPreferenceRead(parameters,"requested").value("parameters").toObject().value("assistant_preferences.instructions").toObject();
        QCOMPARE(current.value("requested").toString(),requested);QVERIFY(!current.contains("saved"));QVERIFY(!current.contains("effective"));
        const auto old=BoundAssistantPreferenceRead(parameters,"saved").value("parameters").toObject().value("assistant_preferences.instructions").toObject();
        QCOMPARE(old.value("saved").toString(),saved);QVERIFY(!old.contains("requested"));
    }
    void invisibleAndBidiControlsCannotEnterSingleLineIdentifiers() {
        okuflow::settings::AssistiveSettings cfg;
        for(const auto control:{char16_t(0x200b),char16_t(0x200e),char16_t(0x200f),char16_t(0x202e),char16_t(0x2066),char16_t(0xfeff)}) {
            const auto text=QStringLiteral("model")+QChar(control);
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.httpModel",text}},false);
            QCOMPARE(plan.error,QString("invalid_argument"));QVERIFY(plan.writes.empty());
        }
        const auto rtl=QStringLiteral("Arabic ")+QChar(0x200f)+QStringLiteral("\nHebrew ")+QChar(0x200e);
        QVERIFY(PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.instructions",rtl}},false).error.isEmpty());
        const auto overrideText=QStringLiteral("text ")+QChar(0x202e);
        QCOMPARE(PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.instructions",overrideText}},false).error,QString("invalid_argument"));
        const auto supplementaryFormat=QString::fromUcs4(U"\U000E0001");
        for(const auto& field:{QString("assistant_preferences.httpModel"),QString("assistant_preferences.instructions")}) {
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},{{field,QString("text")+supplementaryFormat}},false);
            QCOMPARE(plan.error,QString("invalid_argument"));QVERIFY(plan.writes.empty());
        }
    }
    void rateUsesNativeHundredthSteps() {
        okuflow::settings::AssistiveSettings cfg;
        for(const auto& sample:{std::pair{0.333,0.33},std::pair{0.335,0.34},std::pair{-0.335,-0.34},std::pair{1.0,1.0}}) {
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.ttsRate",sample.first}},false);
            QVERIFY(plan.error.isEmpty());QCOMPARE(plan.writes.size(),size_t(1));QCOMPARE(plan.writes[0].value.toDouble(),sample.second);
        }
    }
    void nativeSceneCannotBeResumedByOverlayOrConfiguration() {
        okuflow::settings::AssistiveSettings cfg;
        for(const auto& patch:{QJsonObject{{"assistant_preferences.overlayEnabled",true}},
            QJsonObject{{"assistant_preferences.provider","openai-compatible"}},
            QJsonObject{{"assistant_preferences.instructions","changed"}},
            QJsonObject{{"assistant_preferences.ttsRate",0.333}}}) {
            const auto plan=PrepareAssistantPreferencePatch(cfg,{}, {},patch,false,{},true,false);
            QCOMPARE(plan.error,QString("denied_by_policy"));QVERIFY(plan.writes.empty());
        }
        const auto notes=PrepareAssistantPreferencePatch(cfg,{}, {},{{"notes_output.lectureNotes",false}},true,{},true,false);
        QCOMPARE(notes.error,QString("denied_by_policy"));QVERIFY(notes.writes.empty());
        const auto bounded=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.sceneExplain",true}},false,{},true,false);
        QVERIFY(bounded.error.isEmpty());QVERIFY(bounded.requiresFrames);
        const auto off=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.overlayEnabled",false}},false,{},true,true);
        QVERIFY(off.error.isEmpty());
        const auto ordinary=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.provider","openai-compatible"}},false,{},false,false);
        QVERIFY(ordinary.error.isEmpty());QVERIFY(ordinary.requiresApproval);
        const auto unchanged=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.provider","codex"}},false,{},true,false);
        QVERIFY(unchanged.error.isEmpty());QVERIFY(!unchanged.requiresApproval);
        const auto mixed=PrepareAssistantPreferencePatch(cfg,{}, {},{{"assistant_preferences.provider","openai-compatible"},{"assistant_preferences.sceneExplain",true}},false);
        QCOMPARE(mixed.error,QString("denied_by_policy"));QVERIFY(mixed.writes.empty());
    }
    void oversizedLegacyEditorValuesAreDistinctFromBudgetRetry() {
        const auto finite=QString::fromUcs4(U"\U0001F642").repeated(4096);
        QVERIFY(AssistantPreferenceValueInContract(Field::AssistantInstructions,finite));
        QVERIFY(!AssistantPreferenceValueInContract(Field::AssistantInstructions,finite+"x"));
        QVERIFY(!AssistantPreferenceValueInContract(Field::AssistantHttpModel,QString(257,'x')));
        QVERIFY(!AssistantPreferenceValueInContract(Field::AssistantTtsRate,2.0));
        const auto legacy=QString::fromUcs4(U"\U0001F642").repeated(14000);
        const QJsonObject row{{"requested",legacy},{"effective",legacy},{"saved",legacy},{"available",true}};
        for(const auto& projection:{QString("all"),QString("requested"),QString("saved")}) {
            const auto result=BoundAssistantPreferenceRead({{"assistant_preferences.instructions",row}},projection);
            const auto value=result.value("parameters").toObject().value("assistant_preferences.instructions").toObject();
            QVERIFY(value.value("valueOutOfContract").toBool());QVERIFY(value.value("requiresUi").toBool());
            QVERIFY(!value.value("filterRequired").toBool());QCOMPARE(value.value("unsupportedReason").toString(),QString("value_out_of_range"));
            if(value.contains("requested"))QVERIFY(value.value("requested").isNull());
            if(value.contains("saved"))QVERIFY(value.value("saved").isNull());
            QVERIFY(QJsonDocument(result).toJson(QJsonDocument::Compact).size()<=29*1024);
        }
        QCOMPARE(row.value("requested").toString(),legacy);
        // Recoverable large finite rows must not turn the legacy UI requirement
        // into advice to retry the same impossible legacy projection.
        const QJsonObject mixed{{"assistant_preferences.instructions",QJsonObject{{"requested",legacy},{"effective",finite},{"saved",finite}}},
            {"assistant_preferences.explainPrompt",QJsonObject{{"requested",finite},{"effective",finite},{"saved",finite+"x"}}}};
        const auto packed=BoundAssistantPreferenceRead(mixed);
        const auto preserved=packed.value("parameters").toObject().value("assistant_preferences.instructions").toObject();
        QVERIFY(preserved.value("requiresUi").toBool());QVERIFY(preserved.value("valueOutOfContract").toBool());
        QVERIFY(!preserved.value("filterRequired").toBool());
        QCOMPARE(preserved.value("unsupportedReason").toString(),QString("value_out_of_range"));
        QVERIFY(QJsonDocument(packed).toJson(QJsonDocument::Compact).size()<=29*1024);
    }
};
QTEST_GUILESS_MAIN(AssistantParameterPolicyTests)
#include "assistant_parameter_policy_tests.moc"

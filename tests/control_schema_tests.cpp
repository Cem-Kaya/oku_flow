#include "okuflow/app/agent/control_schema.hpp"
#include <QtTest>
using namespace okuflow::agent;
class ControlSchemaTests final : public QObject {
    Q_OBJECT
private slots:
    void boundedFiniteParameterObjects() {
        const QJsonObject schema{{"type","object"},{"additionalProperties",false},{"minProperties",1},{"maxProperties",2},
            {"properties",QJsonObject{{"sharpness",QJsonObject{{"type","number"},{"minimum",0},{"maximum",1}}},
                                     {"enabled",QJsonObject{{"type","boolean"}}}}}};
        QVERIFY(MatchesControlSchema(QJsonObject{{"enabled",false},{"sharpness",0.25}},schema));
        QVERIFY(!MatchesControlSchema(QJsonObject{},schema));
        QVERIFY(!MatchesControlSchema(QJsonObject{{"enabled",1}},schema));
        QVERIFY(!MatchesControlSchema(QJsonObject{{"sharpness",1.01}},schema));
        QVERIFY(!MatchesControlSchema(QJsonObject{{"shell","command"}},schema));
        QVERIFY(!MatchesControlSchema(QJsonArray{},schema));
    }
    void boundedArraysRejectWrongTypesAndDuplicates() {
        const QJsonObject schema{{"type","array"},{"minItems",1},{"maxItems",2},{"uniqueItems",true},
            {"items",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",3}}}};
        QVERIFY(MatchesControlSchema(QJsonArray{0,3},schema));
        for (const auto value : QJsonArray{QJsonArray{},QJsonArray{1,2,3},QJsonArray{1,1},QJsonArray{1.5},QJsonArray{"1"},QJsonObject{},true})
            QVERIFY(!MatchesControlSchema(value,schema));
        QVERIFY(!MatchesControlSchema(QJsonArray{1},QJsonObject{{"type","array"},{"items",false}}));
    }
    void nullableAndOneOfStayExact() {
        const QJsonObject schema{{"oneOf",QJsonArray{QJsonObject{{"type","null"}},
            QJsonObject{{"type","string"},{"maxLength",4}}}}};
        QVERIFY(MatchesControlSchema(QJsonValue::Null,schema));
        QVERIFY(MatchesControlSchema("safe",schema));
        QVERIFY(!MatchesControlSchema("longer",schema));
        QVERIFY(!MatchesControlSchema(3,schema));
        QVERIFY(!MatchesControlSchema("x",QJsonObject{{"oneOf",QJsonArray{QJsonObject{},QJsonObject{}}}}));
        QVERIFY(!MatchesControlSchema("x",QJsonObject{{"type","unsupported"}}));
        QVERIFY(!MatchesControlSchema("x",QJsonObject{{"type",QJsonArray{"string","null"}}}));
        QVERIFY(!MatchesControlSchema("private",QJsonObject{{"type","string"},{"not",QJsonObject{{"const","private"}}}}));
        QVERIFY(MatchesControlSchema(QString::fromUcs4(U"😀"),QJsonObject{{"type","string"},{"minLength",1},{"maxLength",1}}));
    }
    void recursiveInputsAreBounded() {
        QJsonValue schema = QJsonObject{{"type","boolean"}}, value = true;
        for(int i=0;i<40;++i) {schema=QJsonObject{{"type","array"},{"items",schema}};value=QJsonArray{value};}
        QVERIFY(!MatchesControlSchema(value,schema));
    }
};
QTEST_GUILESS_MAIN(ControlSchemaTests)
#include "control_schema_tests.moc"

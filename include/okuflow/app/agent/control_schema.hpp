#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>

namespace okuflow::agent {
// Finite draft-07 input-schema subset used by the first-party registry.
// Bounded recursion and unknown types fail closed; this never coerces values.
inline bool MatchesControlSchema(const QJsonValue& value,const QJsonValue& definition,int depth=0) {
    if (depth > 32) return false;
    if (definition.isBool()) return definition.toBool();
    if (!definition.isObject()) return false;
    const auto schema = definition.toObject();
    if (schema.contains("oneOf")) {
        if (!schema["oneOf"].isArray() || schema["oneOf"].toArray().isEmpty()) return false;
        int matches = 0;
        for (const auto branch : schema["oneOf"].toArray()) matches += MatchesControlSchema(value,branch,depth+1);
        if (matches != 1) return false;
    }
    if (schema.contains("not") && MatchesControlSchema(value,schema["not"],depth+1)) return false;
    if (schema.contains("const") && value != schema["const"]) return false;
    if (schema.contains("enum") && (!schema["enum"].isArray() || !schema["enum"].toArray().contains(value))) return false;
    const auto type = schema["type"].toString();
    if (schema.contains("type") && !schema["type"].isString()) return false;
    if (type == "object") {
        if (!value.isObject()) return false;
        const auto object = value.toObject();
        if (schema.contains("minProperties") && object.size() < schema["minProperties"].toInt()) return false;
        if (schema.contains("maxProperties") && object.size() > schema["maxProperties"].toInt()) return false;
        const auto properties = schema["properties"].toObject();
        for (const auto required : schema["required"].toArray())
            if (!object.contains(required.toString())) return false;
        for (auto it=object.begin();it!=object.end();++it) {
            if (properties.contains(it.key())) {
                if (!MatchesControlSchema(it.value(),properties[it.key()],depth+1)) return false;
            } else if (schema["additionalProperties"] == false) return false;
            else if (schema["additionalProperties"].isObject() &&
                     !MatchesControlSchema(it.value(),schema["additionalProperties"],depth+1)) return false;
        }
    } else if (type == "array") {
        if (!value.isArray()) return false;
        const auto array = value.toArray();
        if (schema.contains("minItems") && array.size() < schema["minItems"].toInt()) return false;
        if (schema.contains("maxItems") && array.size() > schema["maxItems"].toInt()) return false;
        if (schema.contains("items")) for (const auto item : array)
            if (!MatchesControlSchema(item,schema["items"],depth+1)) return false;
        if (schema["uniqueItems"].toBool()) for (int i=0;i<array.size();++i)
            for (int j=0;j<i;++j) if (array[i] == array[j]) return false;
    } else if (type == "number" || type == "integer") {
        if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
        const auto number = value.toDouble();
        if (type == "integer" && std::floor(number) != number) return false;
        if (schema.contains("minimum") && number < schema["minimum"].toDouble()) return false;
        if (schema.contains("maximum") && number > schema["maximum"].toDouble()) return false;
    } else if (type == "boolean") {
        if (!value.isBool()) return false;
    } else if (type == "null") {
        if (!value.isNull()) return false;
    } else if (type == "string") {
        if (!value.isString()) return false;
        const auto string = value.toString();
        const auto length = string.toUcs4().size();
        if (schema.contains("minLength") && length < schema["minLength"].toInt()) return false;
        if (schema.contains("maxLength") && length > schema["maxLength"].toInt()) return false;
        if (schema.contains("pattern")) {
            const QRegularExpression expression(QRegularExpression::anchoredPattern(schema["pattern"].toString()));
            if (!expression.isValid() || !expression.match(string).hasMatch()) return false;
        }
    } else if (!type.isEmpty()) return false;
    return true;
}
} // namespace okuflow::agent

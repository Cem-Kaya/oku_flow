#include "uct_formula_internal.hpp"
#include "uct_formula_catalog.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <stdexcept>

namespace okuflow::uct::formula {
namespace {
[[noreturn]] void Refuse(const char* reason) { throw std::runtime_error(reason); }
QVector<qint64> Shape(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 4) Refuse("Invalid formula tensor rank.");
    QVector<qint64> result;
    for (const auto& dimension : value.toArray()) {
        const auto number = dimension.toDouble(-1);
        if (!dimension.isDouble() || number < 0 || number > 4096 || number != qint64(number)) Refuse("Invalid formula tensor dimension.");
        result.push_back(qint64(number));
    }
    return result;
}
Profile ParseProfile(const QJsonObject& row, bool state) {
    const auto p = row["profile"].toObject();
    Profile result{row["input"].toString().toUtf8(), row["output"].toString().toUtf8(),
                   p["dtype"].toString().toLatin1(), Shape(p["min"]), Shape(p["opt"]), Shape(p["max"]), {}, {}};
    if (result.input.isEmpty() || result.input.size() > 160 || (state && result.output.isEmpty())) Refuse("Invalid formula tensor name.");
    if (result.minimum.size() != result.maximum.size() || !ShapeWithin(result.optimum, result)) Refuse("Invalid formula tensor profile.");
    TensorBytes(result.maximum, result.dtype);
    if (state) {
        result.initial = Shape(p["initialShape"]);
        const auto hex = row["initialHex"].toString().toLatin1();
        result.initialData = QByteArray::fromHex(hex);
        if (result.initialData.toHex() != hex || !ShapeWithin(result.initial, result) ||
            TensorBytes(result.initial, result.dtype) != result.initialData.size()) Refuse("Invalid formula initial state.");
    }
    return result;
}
QByteArray Read(const QString& root, const QString& name, const QJsonObject& pin, const std::atomic<bool>& cancel) {
    if (cancel.load()) Refuse("Formula setup cancelled.");
    const auto path = QDir(root).filePath(name);
    const QFileInfo info(path);
    QFile file(path);
    const auto bytes = pin["bytes"].toInteger();
    if (!info.isFile() || info.isSymLink() || bytes < 1 || bytes > kPlanLimit || info.size() != bytes || !file.open(QIODevice::ReadOnly))
        Refuse("Formula source missing or outside its pinned size.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray output; output.reserve(bytes);
    while (!file.atEnd()) {
        if (cancel.load()) Refuse("Formula setup cancelled.");
        const auto block = file.read(256 * 1024);
        if (block.isEmpty() || output.size() + block.size() > bytes) Refuse("Cannot read formula source.");
        hash.addData(block); output += block;
    }
    if (output.size() != bytes || file.error() != QFileDevice::NoError ||
        hash.result().toHex() != pin["sha256"].toString().toLatin1()) Refuse("Formula source checksum mismatch.");
    return output;
}
}
QString ModelName(Model model) {
    switch (model) {
    case Model::PlusS: return "PP-FormulaNet_plus-S";
    case Model::PlusM: return "PP-FormulaNet_plus-M";
    case Model::L: return "PP-FormulaNet-L";
    case Model::PlusL: return "PP-FormulaNet_plus-L";
    }
    Refuse("Unknown formula model.");
}
qint64 TensorBytes(const QVector<qint64>& shape, const QByteArray& dtype) {
    qint64 bytes = dtype == "float32" ? 4 : dtype == "int64" ? 8 : dtype == "bool" ? 1 : 0;
    if (!bytes || shape.isEmpty() || shape.size() > 4) Refuse("Unsupported formula tensor type or rank.");
    for (auto dimension : shape) {
        if (dimension < 0 || dimension > 4096 || (dimension && bytes > kBufferLimit / dimension)) Refuse("Formula tensor exceeds its bound.");
        bytes *= dimension;
    }
    return bytes;
}
bool ShapeWithin(const QVector<qint64>& shape, const Profile& p) {
    if (shape.size() != p.minimum.size() || shape.size() != p.maximum.size()) return false;
    for (qsizetype i=0; i<shape.size(); ++i) if (shape[i] < p.minimum[i] || shape[i] > p.maximum[i]) return false;
    return true;
}
Definition Describe(Model model) {
    const auto doc = QJsonDocument::fromJson(kCatalog).object();
    if (doc["schema"].toInt() != 1 || doc["recipe"] != kRecipe) Refuse("Invalid compiled formula catalog.");
    for (const auto& value : doc["models"].toArray()) {
        const auto row = value.toObject();
        if (row["name"] != ModelName(model)) continue;
        Definition result{ModelName(model), row["revision"].toString(), row["maxTokens"].toInt(),
                          row["condition"].toString().toUtf8(), row["tokens"].toString().toUtf8(), {}, {}, {}};
        if (result.maxTokens < 1 || result.maxTokens > 2560 || row["precision"] != "tf32" ||
            result.revision.size() != 40 || result.condition.isEmpty() || result.tokens.isEmpty()) Refuse("Invalid compiled formula identity.");
        QSet<QByteArray> inputs, outputs;
        for (const auto& s : row["states"].toArray()) {
            auto p = ParseProfile(s.toObject(), true);
            if (inputs.contains(p.input) || outputs.contains(p.output)) Refuse("Duplicate formula state.");
            inputs.insert(p.input); outputs.insert(p.output); result.states.push_back(p);
        }
        for (const auto& s : row["invariants"].toArray()) {
            auto p = ParseProfile(s.toObject(), false);
            if (inputs.contains(p.input)) Refuse("Duplicate formula invariant.");
            inputs.insert(p.input); result.invariants.push_back(p);
        }
        if (result.states.isEmpty() || result.states.size() > 21 || result.invariants.isEmpty() || result.invariants.size() > 16 ||
            !outputs.contains(result.tokens)) Refuse("Incomplete formula partition.");
        for (const auto& f : row["files"].toArray()) {
            const auto pin = f.toObject(); const auto name = pin["name"].toString();
            if ((name != "encoder.onnx" && name != "decoder.onnx" && name != "config.json" && name != "README.md") ||
                result.files.contains(name)) Refuse("Invalid formula source name.");
            result.files[name] = pin;
        }
        if (result.files.size() != 4) Refuse("Incomplete formula source pins.");
        return result;
    }
    Refuse("Formula model not in compiled catalog.");
}
QByteArray ReadPinned(const Definition& model, const QString& root, const QString& name, const std::atomic<bool>& cancel) {
    if (!model.files.contains(name)) Refuse("Unpinned formula file.");
    return Read(root, name, model.files[name].toObject(), cancel);
}
void VerifySources(const Definition& model, const QString& root, const std::atomic<bool>& cancel) {
    if (!QDir::isAbsolutePath(root)) Refuse("Formula sources require an absolute directory.");
    for (const auto& name : model.files.keys()) ReadPinned(model, root, name, cancel);
}
QVector<QByteArray> LoadVocabulary(const QByteArray& config) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(config, &error).object();
    if (error.error != QJsonParseError::NoError) Refuse("Invalid formula tokenizer configuration.");
    const auto tokenizer = doc["PostProcess"].toObject()["character_dict"].toObject()["fast_tokenizer_file"].toObject();
    const auto model = tokenizer["model"].toObject();
    if (model["type"] != "BPE" || tokenizer["decoder"].toObject()["type"] != "ByteLevel") Refuse("Unsupported formula tokenizer.");
    // Inverse of the published ByteLevel byte-to-Unicode alphabet. No BPE
    // encoder or tokenizer library is needed to decode generated token IDs.
    QHash<char32_t, char> bytes;
    int extra=0;
    for (int b=0; b<256; ++b) {
        const bool visible=(b>=33 && b<=126) || (b>=161 && b<=172) || b>=174;
        bytes.insert(visible ? char32_t(b) : char32_t(256+extra++), char(b));
    }
    QVector<QByteArray> result(65536);
    QSet<int> seen;
    const auto vocab = model["vocab"].toObject();
    if (vocab.isEmpty() || vocab.size() > result.size()) Refuse("Formula vocabulary exceeds its bound.");
    for (auto i=vocab.begin(); i!=vocab.end(); ++i) {
        const int id=i.value().toInt(-1);
        if (id < 0 || id >= result.size() || seen.contains(id)) Refuse("Invalid formula token ID.");
        seen.insert(id);
        for (const auto c:i.key().toUcs4()) {
            if (!bytes.contains(c)) Refuse("Invalid formula byte token.");
            result[id].push_back(bytes[c]);
        }
    }
    for (const auto& s:tokenizer["added_tokens"].toArray()) {
        const auto row=s.toObject(); const int id=row["id"].toInt(-1);
        if (!row["special"].toBool() || id < 0 || id >= result.size()) Refuse("Invalid formula special token.");
        result[id]=QByteArray(""); // non-null empty = declared special; null = unknown ID
    }
    if (!seen.contains(2)) Refuse("Formula tokenizer has no EOS.");
    return result;
}
QString DecodeTokens(const QVector<qint64>& tokens, const QVector<QByteArray>& vocab) {
    if (tokens.isEmpty() || tokens.size() > 2568) Refuse("Formula token sequence exceeds its bound.");
    QByteArray output;
    bool eos=false;
    for (auto id:tokens) {
        if (id < 0 || id >= vocab.size() || vocab[id].isNull()) Refuse("Formula returned an unknown token.");
        if (id==2) { eos=true; break; }
        output+=vocab[id];
        if (output.size()>32768) Refuse("Formula text exceeds its bound.");
    }
    if (!eos || output.isEmpty()) Refuse("Formula output is incomplete.");
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
    const QString text=decoder(output);
    if (decoder.hasError() || text.contains(QChar::Null) || text.size()>8192) Refuse("Invalid formula output text.");
    return text;
}
QByteArray CacheKey(const Definition& model, const QString& part, const QByteArray& uuid, const QByteArray& runtime) {
    if ((part!="encoder" && part!="decoder") || uuid.size()!=16 || runtime.isEmpty()) Refuse("Invalid formula cache identity.");
    const QByteArray identity=QByteArray(kRecipe)+'\n'+model.name.toUtf8()+'\n'+model.revision.toUtf8()+'\n'+
        QJsonDocument(model.files).toJson(QJsonDocument::Compact)+'\n'+QByteArray(kCatalog)+'\n'+part.toUtf8()+'\n'+uuid.toHex()+'\n'+runtime+
        "\nfp32-tf32-opt2-workspace512MiB-aux0\n";
    return QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex();
}
} // namespace okuflow::uct::formula

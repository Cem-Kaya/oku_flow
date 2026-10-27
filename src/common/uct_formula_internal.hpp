#pragma once
#include "okuflow/common/uct_formula.hpp"
#include <QJsonObject>

namespace okuflow::uct::formula {
inline constexpr char kRecipe[] = "okuflow-formula-tf32-partitions-v1";
inline constexpr qint64 kPlanLimit = 512ll * 1024 * 1024;
inline constexpr qint64 kBufferLimit = 64ll * 1024 * 1024;
inline constexpr qint64 kTotalBufferLimit = 512ll * 1024 * 1024;
struct Profile {
    QByteArray input, output, dtype;
    QVector<qint64> minimum, optimum, maximum, initial;
    QByteArray initialData;
};
struct Definition {
    QString name, revision;
    int maxTokens{};
    QByteArray condition, tokens;
    QVector<Profile> states, invariants;
    QJsonObject files;
};
Definition Describe(Model model);
qint64 TensorBytes(const QVector<qint64>& shape, const QByteArray& dtype);
bool ShapeWithin(const QVector<qint64>& shape, const Profile& profile);
void VerifySources(const Definition& model, const QString& sourceRoot, const std::atomic<bool>& cancelled);
QByteArray ReadPinned(const Definition& model, const QString& sourceRoot, const QString& name,
                      const std::atomic<bool>& cancelled);
QVector<QByteArray> LoadVocabulary(const QByteArray& config);
QString DecodeTokens(const QVector<qint64>& tokens, const QVector<QByteArray>& vocabulary);
QByteArray CacheKey(const Definition& model, const QString& part, const QByteArray& deviceUuid,
                    const QByteArray& runtimeVersion);
} // namespace okuflow::uct::formula

// Every phone link diagnosis has one plain sentence (or deliberately none),
// and every sentence is a translated catalog source in all 24 languages.
#include "okuflow/app/phone_link_text.hpp"

#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QtTest>

#include <map>

using namespace okuflow;
using D = PhoneLinkDiagnosis;

namespace {
std::map<QString, QString> ReadCatalog(const QString& path)
{
    std::map<QString, QString> messages;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return messages;
    QXmlStreamReader xml(&file);
    QString source;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QLatin1String("source")) source = xml.readElementText();
        else if (xml.name() == QLatin1String("translation")) messages[source] = xml.readElementText();
    }
    return messages;
}
} // namespace

class PhoneLinkTextTests : public QObject {
    Q_OBJECT
private slots:
    void everyDiagnosisHasOnePlainSentence()
    {
        for (int i = 0; i < kPhoneLinkDiagnosisCount; ++i) {
            const auto diagnosis = static_cast<D>(i);
            for (const bool userMustAct : {false, true}) {
                const auto text = PhoneLinkPlaceholderText(diagnosis, userMustAct);
                if (diagnosis == D::Connected || diagnosis == D::NotWanted) {
                    QVERIFY(text.title.isEmpty());
                    continue;
                }
                QVERIFY2(!text.title.isEmpty(), PhoneLinkDiagnosisName(diagnosis).data());
                // No process-management or command instructions.
                for (const auto& sentence : {text.title, text.detail}) {
                    QVERIFY(!sentence.contains(QStringLiteral("Task Manager"), Qt::CaseInsensitive));
                    QVERIFY(!sentence.contains(QStringLiteral("adb"), Qt::CaseInsensitive));
                    QVERIFY(!sentence.contains(QStringLiteral("command"), Qt::CaseInsensitive));
                }
            }
        }
        // Rungs still running read as recovery; the user is only asked once
        // the ladder waits for them.
        QCOMPARE(PhoneLinkPlaceholderText(D::PhoneLocked, false).title, QStringLiteral("Restoring USB connection…"));
        QCOMPARE(PhoneLinkPlaceholderText(D::PhoneLocked, true).detail, QStringLiteral("Unlock your phone."));
        QCOMPARE(PhoneLinkPlaceholderText(D::Unauthorized, false).detail,
                 QStringLiteral("Unlock your phone and tap Allow on the USB debugging question."));
        QCOMPARE(PhoneLinkPlaceholderText(D::CompanionPaused, false).detail,
                 QStringLiteral("On the phone, open OkuFlow Camera and tap Resume streaming."));
        QCOMPARE(PhoneLinkPlaceholderText(D::OtherProgramHoldsPhone, false).detail,
                 QStringLiteral("Another program is using the phone's USB connection. Close it or restart the computer."));
    }
    void everySentenceIsTranslatedInEveryCatalog()
    {
        const QDir translations(QStringLiteral(OKUFLOW_TRANSLATIONS_DIR));
        const auto files = translations.entryList({QStringLiteral("okuflow_*.ts")}, QDir::Files);
        QCOMPARE(files.size(), qsizetype{24});
        for (const auto& name : files) {
            const auto catalog = ReadCatalog(translations.filePath(name));
            QVERIFY2(!catalog.empty(), qPrintable(name));
            for (int i = 0; i < kPhoneLinkDiagnosisCount; ++i) {
                for (const bool userMustAct : {false, true}) {
                    const auto text = PhoneLinkPlaceholderText(static_cast<D>(i), userMustAct);
                    for (const auto& sentence : {text.title, text.detail}) {
                        if (sentence.isEmpty()) continue;
                        const auto found = catalog.find(sentence);
                        QVERIFY2(found != catalog.end(), qPrintable(name + QStringLiteral(": ") + sentence));
                        QVERIFY2(!found->second.trimmed().isEmpty(), qPrintable(name + QStringLiteral(": ") + sentence));
                    }
                }
            }
        }
    }
};

QTEST_APPLESS_MAIN(PhoneLinkTextTests)
#include "phone_link_text_tests.moc"

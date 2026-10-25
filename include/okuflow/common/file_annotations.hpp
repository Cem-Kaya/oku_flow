#pragma once
#ifdef _WIN32
#include "okuflow/common/annotation_model.hpp"
#include <QMap>
#include <QString>

namespace okuflow {
using FilePageInk = QMap<int, QVector<AnnotationStroke>>;
struct FileAnnotationCopy {
    QString path, sourceSha256, error;
    FilePageInk pages;
    bool committed{};
};
// Worker-only I/O. One content-bound working copy per original, never the
// original itself. PDF export overlays vector ink without rasterizing pages.
FileAnnotationCopy LoadFileAnnotations(const QString& source, const QString& directory);
FileAnnotationCopy SaveFileAnnotations(const QString& source, const QString& directory,
                                      const FilePageInk& pages, const QString& expectedSourceSha256 = {});
QString FileAnnotationPdfTool();
}
#endif

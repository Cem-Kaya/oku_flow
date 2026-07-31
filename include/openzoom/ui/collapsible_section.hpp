#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QToolButton;
QT_END_NAMESPACE

namespace openzoom {

class CollapsibleSection : public QWidget {
    Q_OBJECT
public:
    explicit CollapsibleSection(const QString& title, QWidget* parent = nullptr);

    QWidget* contentWidget() const { return contentWidget_; }
    void setExpanded(bool expanded);
    bool isExpanded() const;
    void setChangedCount(int count);
    int changedCount() const { return changedCount_; }
    void setPersistKey(const QString& key) { persistKey_ = key; }
    const QString& persistKey() const { return persistKey_; }
    void setSearchExpanded(bool searching);

signals:
    void expandedChanged(bool expanded);

protected:
    void changeEvent(QEvent* event) override;

private:
    void UpdateHeader();

    QString title_;
    QString persistKey_;
    QToolButton* header_{};
    QWidget* contentWidget_{};
    int changedCount_{};
    bool searchExpanded_{};
    bool expandedBeforeSearch_{};
};

} // namespace openzoom

#endif // _WIN32

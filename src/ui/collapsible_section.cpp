#include "openzoom/ui/collapsible_section.hpp"

#ifdef _WIN32

#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace openzoom {

CollapsibleSection::CollapsibleSection(const QString& title, QWidget* parent)
    : QWidget(parent), title_(title)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    header_ = new QToolButton(this);
    header_->setObjectName(QStringLiteral("collapsibleHeader"));
    header_->setCheckable(true);
    header_->setChecked(true);
    header_->setArrowType(Qt::DownArrow);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header_->setAccessibleDescription(
        QStringLiteral("Section heading. Activate to expand or collapse this settings group."));
    layout->addWidget(header_);

    contentWidget_ = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(contentWidget_);
    contentLayout->setContentsMargins(12, 0, 0, 4);
    contentLayout->setSpacing(8);
    layout->addWidget(contentWidget_);

    connect(header_, &QToolButton::toggled, this, [this](bool expanded) {
        contentWidget_->setVisible(expanded);
        header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        UpdateHeader();
        if (!searchExpanded_) {
            emit expandedChanged(expanded);
        }
    });
    UpdateHeader();
}

void CollapsibleSection::setExpanded(bool expanded)
{
    header_->setChecked(expanded);
    contentWidget_->setVisible(expanded);
    header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    UpdateHeader();
}

bool CollapsibleSection::isExpanded() const
{
    return header_ && header_->isChecked();
}

void CollapsibleSection::setChangedCount(int count)
{
    changedCount_ = std::max(0, count);
    UpdateHeader();
}

void CollapsibleSection::setSearchExpanded(bool searching)
{
    if (searching == searchExpanded_) {
        return;
    }
    if (searching) {
        expandedBeforeSearch_ = isExpanded();
        searchExpanded_ = true;
        setExpanded(true);
    } else {
        searchExpanded_ = false;
        setExpanded(expandedBeforeSearch_);
    }
}

void CollapsibleSection::UpdateHeader()
{
    QString label = title_;
    if (changedCount_ > 0) {
        label += QStringLiteral(" \u00b7 %1 changed").arg(changedCount_);
    }
    header_->setText(label);
    header_->setAccessibleName(
        QStringLiteral("%1, heading, %2%3")
            .arg(title_,
                 isExpanded() ? QStringLiteral("expanded")
                              : QStringLiteral("collapsed"),
                 changedCount_ > 0
                     ? QStringLiteral(", %1 changed").arg(changedCount_)
                     : QString()));
}

} // namespace openzoom

#endif // _WIN32

// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#ifndef WRAPPEDLABELS_H
#define WRAPPEDLABELS_H
#include <QLabel>
#include <climits>

//! Gives every word-wrapped label under root the height its text needs at the
//! label's current width, so a longer translation or a larger font is not cut.
//! heightForWidth() would not do: it includes the previous minimum, so it cannot
//! shrink a minimum measured before the form reached its actual width.
//! Call it from the dialog's event() on LayoutRequest, Resize and Show.
inline void fitWrappedLabels(const QWidget &root)
{
    for (auto *label : root.findChildren<QLabel *>()) {
        if (!label->wordWrap())
            continue;
        const int width = qMax(1, label->contentsRect().width() - 2 * label->margin());
        const int height = label->text().isEmpty()
            ? 0
            : label->fontMetrics()
                  .boundingRect(QRect(0, 0, width, INT_MAX), Qt::TextWordWrap, label->text())
                  .height();
        label->setMinimumHeight(height + 2 * label->margin());
    }
}

#endif // WRAPPEDLABELS_H

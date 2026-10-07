// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#ifndef EXTERNALTOOLPATHWIDGET_H
#define EXTERNALTOOLPATHWIDGET_H
#include <QWidget>
#include "tools/ExternalToolResolver.h"
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QToolButton;
class ExternalToolPathWidget : public QWidget
{
    Q_OBJECT
public:
    ExternalToolPathWidget(const QString &tool,
                           externaltool::PathMode mode,
                           const QString &path,
                           const QString &objectPrefix,
                           QWidget *parent);
    //! The user picked another engine: back to automatic discovery.
    void selectTool(const QString &tool);
    //! Changes the engine Automatic looks for, keeping the mode and custom path.
    void setTool(const QString &tool);
    externaltool::PathMode mode() const;
    QString customPath() const;
    QLineEdit *editor() const { return _path; }
signals:
    void selectionChanged();

private:
    void refresh();
    QString _tool, _customPath;
    QComboBox *_mode;
    QLineEdit *_path;
    QLabel *_status;
    QPushButton *_browse;
    QToolButton *_details;
    bool _updating = false;
};
#endif

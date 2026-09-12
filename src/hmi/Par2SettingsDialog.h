// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#ifndef PAR2SETTINGSDIALOG_H
#define PAR2SETTINGSDIALOG_H
#include <QDialog>
#include <QFileInfoList>
#include <QVector>
#include <atomic>
#include <memory>
#include "par2/Par2Settings.h"

class NgPost;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QDialogButtonBox;
class QProcess;

class Par2SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit Par2SettingsDialog(NgPost *ngPost, const QFileInfoList &files = {},
                                bool beforeCompression = false, bool percentageOverride = false,
                                QWidget *parent = nullptr);
    ~Par2SettingsDialog() override;
    void accept() override;
private:
    NgPost *_ngPost;
    par2::Settings _initial;
    QComboBox *_tool, *_blocks, *_volumes, *_distribution, *_device;
    QLineEdit *_path;
    QSpinBox *_percentage, *_blockCount, *_volumeCount, *_threads, *_memory;
    QDoubleSpinBox *_blockBytes, *_volumeMiB;
    QCheckBox *_gpu, *_custom;
    QPlainTextEdit *_arguments;
    QLabel *_preview, *_status, *_memoryLabel, *_toolStatus, *_argumentsPreview;
    QDialogButtonBox *_buttons;
    QProcess *_probe = nullptr;
    QProcess *_gpuProbe = nullptr;
    QVector<qint64> _sizes;
    std::shared_ptr<std::atomic_bool> _cancelScan;
    bool _beforeCompression, _percentageOverride;
    bool _dirty = false, _pathDirty = false, _loading = true, _scanIncomplete = false;
    bool _threadsSupported = true;
    QString _probedPath;
    par2::Tool selectedTool() const;
    par2::Tool effectiveTool() const;
    par2::Settings settings() const;
    void changed();
    void updateControls();
    void updatePreview();
    void selectTool();
    void probeTool();
    void findGpus();
};
#endif

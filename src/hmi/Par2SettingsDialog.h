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
class ExternalToolPathWidget;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QDialogButtonBox;
class QFormLayout;
class QHBoxLayout;
class QToolButton;
class QProcess;
class QPushButton;

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
    void _buildToolControls(QFormLayout *form);
    void _buildRecoveryControls(QFormLayout *form);
    void _buildAdvancedControls(QFormLayout *details);
    void _loadSettings(QToolButton *advancedButton);
    void _connectControls();
    void _scanFiles(const QFileInfoList &files);
    bool event(QEvent *event) override;
    bool _layoutReady = false;
    NgPost *_ngPost;
    par2::Settings _initial;
    QComboBox *_tool, *_blocks, *_volumes, *_distribution, *_device;
    QLineEdit *_path;
    ExternalToolPathWidget *_toolPath;
    QSpinBox *_percentage, *_blockCount, *_volumeCount, *_threads, *_memory;
    QDoubleSpinBox *_blockBytes, *_volumeMiB;
    QCheckBox *_gpu, *_custom;
    QPlainTextEdit *_arguments;
    QLabel *_preview, *_status, *_memoryLabel, *_toolStatus, *_argumentsPreview;
    QDialogButtonBox *_buttons;
    QPushButton *_findGpuButton;
    QFormLayout *_form = nullptr;      //!< the basic section, whose GPU rows come and go with the tool
    QHBoxLayout *_deviceRow = nullptr;
    QProcess *_probe = nullptr;
    QProcess *_gpuProbe = nullptr;
    QVector<qint64> _sizes;
    std::shared_ptr<std::atomic_bool> _cancelScan;
    bool _beforeCompression, _percentageOverride;
    bool _dirty = false, _loading = true, _scanIncomplete = false, _scanning = false;
    bool _threadsSupported = true;
    QString _probedPath;
    par2::Tool _probedTool = par2::Tool::Auto;
    //! What the last "--opencl-list" said. ParPar exits 1 without writing a
    //! single file when GPU work is requested and no device answers, which
    //! aborts the whole post, so Save has to refuse that combination.
    enum class GpuScan { Unknown, Running, Failed, None, NoGpu, Found };
    GpuScan _gpuScan = GpuScan::Unknown;
    par2::Tool selectedTool() const;
    par2::Tool effectiveTool() const;
    par2::Settings settings() const;
    void changed();
    void updateControls();
    void _updateChoiceControls(bool manual, par2::Tool tool, par2::Volumes volumes);
    void _updateResourceControls(bool manual, par2::Tool tool);
    void _allowToolChoices(par2::Tool tool, par2::Volumes volumes);
    QString _gpuScanError() const;
    QString _controlsError(const par2::Settings &s, bool manual, par2::Tool tool) const;
    void updatePreview();
    bool _splitIntoVolumes(QVector<qint64> &sizes);
    void _showInvalidEstimate(const par2::Settings &configured, const QVector<qint64> &sizes);
    QString _estimateText(const par2::Estimate &e) const;
    void selectTool();
    void probeTool();
    void resetGpuScan();
    void findGpus();
};
#endif

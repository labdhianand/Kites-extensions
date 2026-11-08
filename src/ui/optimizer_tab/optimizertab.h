#pragma once
#include "benchmarking/suggestion.h"
#include "processor/processor_types.h"
#include "ui/common/kitestab.h"

#include <QFutureWatcher>
#include <QString>
#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace Kites
{
class OptimizerEditor;

/**
 * @brief The Optimizer tab: suggests a better instruction order for the program in the editor,
 * shows it side by side with the original, and reports how both measure on the simulator.
 *
 * The suggestion is computed on a background thread, since measuring it runs the simulator.
 */
class OptimizerTab : public KitesTab
{
    Q_OBJECT
  public:
    explicit OptimizerTab(QWidget *parent = nullptr);

    /// Where the tab reads the program from (the Editor tab's text).
    void setSourceProvider(std::function<QString()> provider);
    /// Where edits made in the original pane go back to (the Editor tab). Returns false if the
    /// editor cannot take them right now (a program is running).
    void setSourceSink(std::function<bool(const QString &)> sink);
    /// What "Apply to editor" does with the suggested program; false if it could not.
    void setApplyHandler(std::function<bool(const QString &)> handler);
    /// Preselects the pipeline in use, when it is one the optimizer works for.
    void setProcessor(ProcessorType processor);

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private slots:
    void suggest();
    void suggestionFinished();
    void applySuggestion();
    void memoryChanged();
    void refreshColours();
    void originalEdited();

  private:
    struct Outcome
    {
        QString original;
        benchmarking::Suggestion suggestion;
        QString error;
    };

    QWidget *buildControls();
    QWidget *buildResults();
    QWidget *metricCard(QLabel *&value, const QString &label);
    void loadOriginal(const QString &text);
    void syncToEditor();
    void markStale();
    void clearResults(const QString &note);
    void setBusy(bool busy);

    std::function<QString()> m_sourceProvider;
    std::function<bool(const QString &)> m_sourceSink;
    std::function<bool(const QString &)> m_applyHandler;

    QComboBox *m_pipeline{nullptr};
    QComboBox *m_memory{nullptr};
    QComboBox *m_unroll{nullptr};
    QSpinBox *m_plannedLatency{nullptr};
    QPushButton *m_suggestButton{nullptr};
    QPushButton *m_applyButton{nullptr};

    OptimizerEditor *m_originalView{nullptr};
    OptimizerEditor *m_suggestedView{nullptr};

    QLabel *m_originalCycles{nullptr};
    QLabel *m_suggestedCycles{nullptr};
    QLabel *m_change{nullptr};
    QLabel *m_result{nullptr};
    QLabel *m_originalCyclesLabel{nullptr};
    QLabel *m_notes{nullptr};

    QFutureWatcher<Outcome> m_watcher;
    QString m_suggestedSource;
    bool m_improved{false};
    bool m_correct{false};
    bool m_hasError{false};
    bool m_loading{false};        ///< the original pane is being filled in by code, not typed
    bool m_originalEdited{false}; ///< typed in the original pane, not yet in the Editor tab
    bool m_stale{false};          ///< the program changed after the suggestion on screen
};
} // namespace Kites

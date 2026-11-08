#include "optimizertab.h"

#include "custom_pseudo_manager/custom_pseudo_manager.h"
#include "optimizer/loop_unroller.h"
#include "optimizer/source_text.h"
#include "optimizer_editor.h"
#include "ui/theme/theme_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace Kites
{
namespace
{
QLabel *caption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("optimizerCaption");
    return label;
}

QString percentChange(unsigned before, unsigned after)
{
    const double change = 100.0 * (static_cast<double>(after) - before) / before;
    return QString("%1%2%").arg(change > 0 ? "+" : "").arg(change, 0, 'f', 1);
}
} // namespace

OptimizerTab::OptimizerTab(QWidget *parent) : KitesTab(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(buildControls());

    // The original and the suggestion, side by side.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(1);
    auto addPane = [&](const QString &title, OptimizerEditor *&view, OptimizerEditor::Tint tint,
                       bool editable)
    {
        auto *pane = new QWidget(splitter);
        auto *paneLayout = new QVBoxLayout(pane);
        paneLayout->setContentsMargins(0, 0, 0, 0);
        paneLayout->setSpacing(0);
        paneLayout->addWidget(caption(title, pane));
        view = new OptimizerEditor(tint, editable, pane);
        paneLayout->addWidget(view, 1);
        splitter->addWidget(pane);
    };
    addPane("ORIGINAL", m_originalView, OptimizerEditor::Tint::Removed, true);
    addPane("SUGGESTED", m_suggestedView, OptimizerEditor::Tint::Added, false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    layout->addWidget(buildResults());

    clearResults("Write or open a program, choose a pipeline and press Suggest. Edits made here "
                 "also appear in the Editor tab.");
    m_originalView->setPlaceholderText("Type your program here...");
    m_suggestedView->setPlaceholderText("The suggested program appears here.");
    connect(m_originalView, &QPlainTextEdit::textChanged, this, &OptimizerTab::originalEdited);

    connect(&m_watcher, &QFutureWatcher<Outcome>::finished, this,
            &OptimizerTab::suggestionFinished);
    connect(&ThemeManager::getInstance(), &ThemeManager::themeChangedSignal, this,
            &OptimizerTab::refreshColours);
    connect(&ThemeManager::getInstance(), &ThemeManager::editorThemeChangedSignal, this,
            &OptimizerTab::refreshColours);
}

QWidget *OptimizerTab::buildControls()
{
    auto *bar = new QFrame(this);
    bar->setObjectName("optimizerControls");
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(6);

    auto addField = [&](const QString &label, QWidget *field)
    {
        row->addWidget(new QLabel(label, bar));
        row->addWidget(field);
        row->addSpacing(10);
    };

    m_pipeline = new QComboBox(bar);
    m_pipeline->addItem("Stalls + forwarding",
                        static_cast<int>(ProcessorType::RV5Stage_H_F));
    m_pipeline->addItem("Stalls, no forwarding",
                        static_cast<int>(ProcessorType::RV5Stage_H_NF));
    m_pipeline->addItem("No stalls + forwarding",
                        static_cast<int>(ProcessorType::RV5Stage_NH_F));
    m_pipeline->addItem("No stalls, no forwarding",
                        static_cast<int>(ProcessorType::RV5Stage_NH_NF));
    m_pipeline->setToolTip("The 5-stage pipeline to optimize for and measure on.\n"
                           "Stalls: the pipeline has hazard detection and waits on its own.\n"
                           "No stalls: the program must leave the gaps itself (NOPs).");
    m_pipeline->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    addField("Pipeline", m_pipeline);

    m_memory = new QComboBox(bar);
    m_memory->addItem("Ideal");
    m_memory->addItem("Cold cache");
    m_memory->setToolTip("Ideal: every access takes one cycle.\nCold cache: L1 128 B, L2 2 KB; "
                         "a load costs +0 / +4 / +20 cycles from L1 / L2 / memory.");
    addField("Memory", m_memory);

    m_unroll = new QComboBox(bar);
    m_unroll->addItem("Off", 1);
    m_unroll->addItem("Up to x2", 2);
    m_unroll->addItem("Up to x4", 4);
    m_unroll->setToolTip("Unroll counted loops so their iterations can overlap.\nOnly loops with a "
                         "fixed trip count that the factor divides are unrolled.");
    row->addWidget(new QLabel("Unroll", bar));
    row->addWidget(m_unroll);
    m_onlyLiveRegisters = new QCheckBox("Fold addresses", bar);
    m_onlyLiveRegisters->setToolTip(
        "Let the unrolled copies share one address computation, so a later copy reaches its data\n"
        "with a larger offset (lw x5, 4(x30)) instead of recomputing the address.\n\n"
        "Registers nothing reads after the loop (like the old index-times-4 temporary) may then\n"
        "end with different values. Memory and every register read later still have to match,\n"
        "and the registers that end differently are listed.");
    m_onlyLiveRegisters->setEnabled(false);
    row->addWidget(m_onlyLiveRegisters);
    row->addSpacing(10);
    connect(m_unroll, &QComboBox::currentIndexChanged, this,
            [this]() { m_onlyLiveRegisters->setEnabled(m_unroll->currentData().toInt() > 1); });

    m_plannedLatency = new QSpinBox(bar);
    m_plannedLatency->setRange(0, 40);
    m_plannedLatency->setSuffix(" cycles");
    m_plannedLatency->setToolTip("Extra cycles to plan for on every load.\nOnly matters on "
                                 "pipelines with hazard detection, with the cold cache.");
    addField("Load latency", m_plannedLatency);

    row->addStretch(1);

    m_applyButton = new QPushButton("Apply to editor", bar);
    m_applyButton->setEnabled(false);
    m_applyButton->setToolTip("Replace the program in the editor with the suggestion");
    row->addWidget(m_applyButton);

    m_suggestButton = new QPushButton("Suggest", bar);
    m_suggestButton->setDefault(true);
    row->addWidget(m_suggestButton);

    connect(m_suggestButton, &QPushButton::clicked, this, &OptimizerTab::suggest);
    connect(m_applyButton, &QPushButton::clicked, this, &OptimizerTab::applySuggestion);
    connect(m_memory, &QComboBox::currentIndexChanged, this, &OptimizerTab::memoryChanged);
    return bar;
}

QWidget *OptimizerTab::metricCard(QLabel *&value, const QString &label)
{
    auto *card = new QFrame(this);
    card->setObjectName("metricCard");
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 8, 14, 8);
    cardLayout->setSpacing(0);
    value = new QLabel("–", card);
    value->setObjectName("metricValue");
    auto *name = new QLabel(label, card);
    name->setObjectName("metricLabel");
    cardLayout->addWidget(value);
    cardLayout->addWidget(name);
    return card;
}

QWidget *OptimizerTab::buildResults()
{
    auto *panel = new QWidget(this);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(10, 10, 10, 10);
    panelLayout->setSpacing(8);

    auto *cards = new QHBoxLayout();
    cards->setSpacing(10);
    QWidget *original = metricCard(m_originalCycles, "original, cycles");
    m_originalCyclesLabel = original->findChild<QLabel *>("metricLabel");
    cards->addWidget(original, 1);
    cards->addWidget(metricCard(m_suggestedCycles, "suggested, cycles"), 1);
    cards->addWidget(metricCard(m_change, "change"), 1);
    cards->addWidget(metricCard(m_result, "result"), 1);
    panelLayout->addLayout(cards);

    m_notes = new QLabel(panel);
    m_notes->setObjectName("metricLabel");
    m_notes->setWordWrap(true);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    panelLayout->addWidget(m_notes);
    return panel;
}

void OptimizerTab::setSourceProvider(std::function<QString()> provider)
{
    m_sourceProvider = std::move(provider);
}

void OptimizerTab::setSourceSink(std::function<bool(const QString &)> sink)
{
    m_sourceSink = std::move(sink);
}

void OptimizerTab::setApplyHandler(std::function<bool(const QString &)> handler)
{
    m_applyHandler = std::move(handler);
}

void OptimizerTab::setProcessor(ProcessorType processor)
{
    const int index = m_pipeline->findData(static_cast<int>(processor));
    if (index >= 0)
    {
        m_pipeline->setCurrentIndex(index);
    }
}

void OptimizerTab::showEvent(QShowEvent *event)
{
    KitesTab::showEvent(event);
    // Pick up whatever changed in the Editor tab while this tab was hidden. When nothing did, the
    // pane is left alone, so its cursor and undo history survive switching tabs. Edits that could
    // not be handed to the editor yet (it was locked by a run) are never overwritten.
    if (m_sourceProvider && !m_originalEdited)
    {
        const QString source = m_sourceProvider();
        if (source != m_originalView->toPlainText())
        {
            loadOriginal(source);
            if (!m_suggestedSource.isEmpty())
            {
                markStale();
            }
        }
    }
}

void OptimizerTab::hideEvent(QHideEvent *event)
{
    syncToEditor();
    KitesTab::hideEvent(event);
}

void OptimizerTab::syncToEditor()
{
    // One undoable edit in the Editor tab for everything typed here.
    if (m_originalEdited && m_sourceSink && m_sourceSink(m_originalView->toPlainText()))
    {
        m_originalEdited = false;
    }
}

void OptimizerTab::loadOriginal(const QString &text)
{
    m_loading = true;
    m_originalView->showProgram(text, {});
    m_loading = false;
    m_originalEdited = false;
}

void OptimizerTab::originalEdited()
{
    if (m_loading)
    {
        return;
    }
    m_originalEdited = true;
    if (!m_suggestedSource.isEmpty() && !m_stale)
    {
        markStale();
    }
}

void OptimizerTab::markStale()
{
    // The suggestion on screen is for an older program: keep it visible, but do not tint lines
    // that no longer match, and do not let it be applied over the new program.
    m_stale = true;
    m_originalView->setTintedLines({});
    m_suggestedView->setTintedLines({});
    m_applyButton->setEnabled(false);
    m_notes->setText("The program has changed since this suggestion: press Suggest to update it.");
}

void OptimizerTab::memoryChanged()
{
    // Plan for an L2 hit by default with the cold cache: it is the right guess for most loads.
    m_plannedLatency->setValue(m_memory->currentIndex() == 1 ? benchmarking::kColdCacheL2Latency
                                                             : 0);
}

void OptimizerTab::setBusy(bool busy)
{
    m_suggestButton->setEnabled(!busy);
    m_suggestButton->setText(busy ? "Measuring..." : "Suggest");
    m_pipeline->setEnabled(!busy);
    m_memory->setEnabled(!busy);
    m_unroll->setEnabled(!busy);
    m_onlyLiveRegisters->setEnabled(!busy && m_unroll->currentData().toInt() > 1);
    m_plannedLatency->setEnabled(!busy);
    if (busy)
    {
        m_applyButton->setEnabled(false);
    }
}

void OptimizerTab::clearResults(const QString &note)
{
    for (QLabel *label : {m_originalCycles, m_suggestedCycles, m_change, m_result})
    {
        label->setText("–");
    }
    m_originalCyclesLabel->setText("original, cycles");
    m_suggestedSource.clear();
    m_applyButton->setEnabled(false);
    m_stale = false;
    m_hasError = false;
    m_notes->setText(note);
    refreshColours();
}

void OptimizerTab::suggest()
{
    if (m_watcher.isRunning())
    {
        return;
    }
    const QString source = m_originalView->toPlainText();
    if (source.trimmed().isEmpty())
    {
        clearResults("The program is empty: write one on the left, or open one from the File menu.");
        return;
    }

    benchmarking::SuggestionRequest request;
    try
    {
        // The same expansion of custom pseudo-instructions that Run does.
        request.source = customPseudoManager::expandPseudoInstruction(source.toStdString());
    }
    catch (const std::exception &error)
    {
        clearResults("Could not expand the pseudo-instructions: " + QString(error.what()));
        m_hasError = true;
        refreshColours();
        return;
    }
    request.processor = static_cast<ProcessorType>(m_pipeline->currentData().toInt());
    request.memory = m_memory->currentIndex() == 1 ? benchmarking::MemorySetup::ColdCache
                                                   : benchmarking::MemorySetup::Ideal;
    request.unrollFactor = m_unroll->currentData().toInt();
    request.allowDeadRegisterChanges =
        request.unrollFactor > 1 && m_onlyLiveRegisters->isChecked();
    request.plannedLatency = m_plannedLatency->value();

    m_originalView->setTintedLines({});
    m_suggestedView->showProgram(QString(), {});
    clearResults("Optimizing and measuring both programs on the simulator...");
    setBusy(true);

    m_watcher.setFuture(QtConcurrent::run(
        [request, source]()
        {
            Outcome outcome;
            outcome.original = source;
            try
            {
                outcome.suggestion = benchmarking::makeSuggestion(request);
            }
            catch (const std::exception &error)
            {
                outcome.error = QString::fromStdString(error.what());
            }
            return outcome;
        }));
}

void OptimizerTab::suggestionFinished()
{
    setBusy(false);
    const Outcome outcome = m_watcher.result();
    if (!outcome.error.isEmpty())
    {
        clearResults("Could not optimize this program: " + outcome.error);
        m_hasError = true;
        refreshColours();
        return;
    }

    const benchmarking::Suggestion &suggestion = outcome.suggestion;
    const QString suggested = QString::fromStdString(suggestion.result.source);
    const optimizer::LineChanges changes =
        optimizer::changedLines(outcome.original.toStdString(), suggestion.result.source);
    m_originalView->setTintedLines(changes.removedLines);
    m_suggestedView->showProgram(suggested, changes.addedLines);
    m_suggestedSource = suggested;
    m_applyButton->setEnabled(suggested != outcome.original);

    // What changed and why, in one short paragraph.
    QStringList notes;
    if (suggestion.paddingAdded > 0)
    {
        notes << QString("This pipeline does not stall on hazards, so the program was first padded "
                         "with %1 NOPs to compute the right result; the original figures are for "
                         "that padded program.")
                     .arg(suggestion.paddingAdded);
    }
    int reordered = 0;
    QStringList leftAlone;
    for (const optimizer::BlockSummary &block : suggestion.result.blocks)
    {
        reordered += block.changed ? 1 : 0;
        if (!block.changed && !block.reason.empty())
        {
            leftAlone << QString("lines %1-%2: %3")
                             .arg(block.firstLine)
                             .arg(block.lastLine)
                             .arg(QString::fromStdString(block.reason));
        }
    }
    notes << QString("%1 of %2 basic blocks reordered.").arg(reordered).arg(suggestion.result.blocks.size());
    for (const optimizer::LoopReport &loop : suggestion.result.loops)
    {
        notes << (loop.unrolled ? QString("Loop at lines %1-%2 unrolled x%3 (%4 iterations).")
                                      .arg(loop.firstLine)
                                      .arg(loop.lastLine)
                                      .arg(loop.factor)
                                      .arg(loop.tripCount)
                                : QString("Loop at lines %1-%2 not unrolled: %3.")
                                      .arg(loop.firstLine)
                                      .arg(loop.lastLine)
                                      .arg(QString::fromStdString(loop.reason)));
        if (loop.foldedInstructions > 0)
        {
            notes << QString("%1 repeated address instructions removed.")
                         .arg(loop.foldedInstructions);
        }
        if (!loop.changedDeadRegisters.empty())
        {
            QStringList changed;
            for (unsigned reg : loop.changedDeadRegisters)
            {
                changed << QString("x%1").arg(reg);
            }
            notes << QString("%1 now end differently: nothing reads them after the loop, so they "
                             "are not compared.")
                         .arg(changed.join(", "));
        }
        QStringList saved;
        for (unsigned reg : loop.renamedTo)
        {
            if (optimizer::isCalleeSaved(reg))
            {
                saved << QString("x%1").arg(reg);
            }
        }
        if (!saved.isEmpty())
        {
            notes << QString("No temporary register was free, so callee-saved %1 is used: inside a "
                             "function, save and restore it.")
                         .arg(saved.join(", "));
        }
    }
    if (!leftAlone.isEmpty())
    {
        notes << "Left as is: " + leftAlone.join("; ") + ".";
    }
    if (!suggestion.result.changed)
    {
        notes << "No better order was found.";
    }
    m_notes->setText(notes.join("  "));

    m_originalCyclesLabel->setText(suggestion.paddingAdded > 0 ? "padded original, cycles"
                                                               : "original, cycles");
    m_originalCycles->setText(QString::number(suggestion.baselineCycles));
    m_suggestedCycles->setText(QString::number(suggestion.suggestionCycles));
    m_change->setText(percentChange(suggestion.baselineCycles, suggestion.suggestionCycles));
    m_result->setText(suggestion.suggestionCorrect ? "Identical" : "Different");
    m_result->setToolTip(suggestion.suggestionCorrect
                             ? "The suggestion leaves the same registers and memory as the "
                               "original on the single-cycle processor."
                             : "The suggestion computed a different result. Please report this.");
    m_improved = suggestion.suggestionCycles < suggestion.baselineCycles;
    m_correct = suggestion.suggestionCorrect;
    refreshColours();

    // Typed in the original pane while this was being measured.
    if (m_originalView->toPlainText() != outcome.original)
    {
        markStale();
    }
}

void OptimizerTab::applySuggestion()
{
    if (m_suggestedSource.isEmpty() || !m_applyHandler)
    {
        return;
    }
    const auto answer = QMessageBox::question(
        this, "Apply suggestion",
        "Replace the program in the editor with the suggested one?");
    if (answer != QMessageBox::Yes)
    {
        return;
    }
    const QString suggested = m_suggestedSource;
    if (!m_applyHandler(suggested))
    {
        QMessageBox::information(this, "Apply suggestion",
                                 "The editor is locked while a program runs. Stop it and try again.");
        return;
    }
    loadOriginal(suggested);
    m_suggestedView->showProgram(QString(), {});
    clearResults("Applied: the editor now holds the suggested program. Ctrl+Z in the Editor tab "
                 "brings the old one back.");
}

void OptimizerTab::refreshColours()
{
    const ThemeManager &theme = ThemeManager::getInstance();
    const QString good = theme.getEditorSuccessColor().name();
    const QString bad = theme.getEditorErrorColor().name();
    const bool haveResult = !m_suggestedSource.isEmpty();

    m_change->setStyleSheet(haveResult && m_improved ? "color: " + good + ";" : QString());
    m_result->setStyleSheet(haveResult ? "color: " + (m_correct ? good : bad) + ";" : QString());
    m_notes->setStyleSheet(m_hasError ? "color: " + bad + ";" : QString());
}
} // namespace Kites

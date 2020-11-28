#pragma once
#include "debuggermodels.h"
#include "ui_debuggerwindow.h"
#include <QtWidgets/QMainWindow>
#include <memory>
#include <optional>

class DebuggerWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit DebuggerWindow(QWidget* parent = nullptr);
  ~DebuggerWindow();

public Q_SLOTS:
  void onEmulationPaused(bool paused);
  void onDebuggerMessageReported(const QString& message);

  void refreshAll();

  void scrollToPC();

  void onCloseActionTriggered();
  void onPauseActionToggled(bool paused);
  void onRunToCursorTriggered();
  void onGoToPCTriggered();
  void onGoToAddressTriggered();
  void onDumpAddressTriggered();
  void onFollowAddressTriggered();
  void onToggleBreakpointTriggered();
  void onStepIntoActionTriggered();
  void onStepOverActionTriggered();
  void onStepOutActionTriggered();
  void onCodeViewItemActivated(QModelIndex index);

private:
  enum class ActiveMemoryRegion
  {
    RAM,
    Scratchpad,
    EXP1,
    Count
  };

  void setupAdditionalUi();
  void connectSignals();
  void disconnectSignals();
  void createModels();
  void setUIEnabled(bool enabled);
  void setActiveMemoryRegion(ActiveMemoryRegion region);
  void toggleBreakpoint(VirtualMemoryAddress address);
  std::optional<VirtualMemoryAddress> getSelectedCodeAddress();
  bool tryFollowLoadStore(VirtualMemoryAddress address);
  void scrollToCodeAddress(VirtualMemoryAddress address);
  void scrollToMemoryAddress(VirtualMemoryAddress address);
  void refreshBreakpointList();

  Ui::DebuggerWindow m_ui;

  std::unique_ptr<DebuggerCodeModel> m_code_model;
  std::unique_ptr<DebuggerRegistersModel> m_registers_model;
  std::unique_ptr<DebuggerStackModel> m_stack_model;

  ActiveMemoryRegion m_active_memory_region = ActiveMemoryRegion::Count;
};

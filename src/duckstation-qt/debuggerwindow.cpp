#include "debuggerwindow.h"
#include "core/cpu_core_private.h"
#include "qthostinterface.h"
#include <QtGui/QFontDatabase>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>

DebuggerWindow::DebuggerWindow(QWidget* parent /* = nullptr */) : QMainWindow(parent)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  createModels();
  setActiveMemoryRegion(ActiveMemoryRegion::RAM);
  setUIEnabled(false);
}

DebuggerWindow::~DebuggerWindow() {}

void DebuggerWindow::onEmulationPaused(bool paused)
{
  if (paused)
  {
    setUIEnabled(true);
    refreshAll();
    refreshBreakpointList();
  }
  else
  {
    setUIEnabled(false);
  }

  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(paused);
  }
}

void DebuggerWindow::onDebuggerMessageReported(const QString& message)
{
  m_ui.statusbar->showMessage(message, 0);
}

void DebuggerWindow::refreshAll()
{
  m_registers_model->invalidateView();
  m_stack_model->invalidateView();
  m_ui.memoryView->repaint();

  m_code_model->setPC(CPU::g_state.regs.pc);
  scrollToPC();
}

void DebuggerWindow::scrollToPC()
{
  return scrollToCodeAddress(CPU::g_state.regs.pc);
}

void DebuggerWindow::scrollToCodeAddress(VirtualMemoryAddress address)
{
  int row = m_code_model->getRowForAddress(address);
  if (row >= 0)
  {
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    m_ui.codeView->scrollTo(m_code_model->index(row, 0));
  }
}

void DebuggerWindow::onCloseActionTriggered()
{
  //
}

void DebuggerWindow::onPauseActionToggled(bool paused)
{
  if (!paused)
  {
    m_registers_model->saveCurrentValues();
    setUIEnabled(false);
  }

  QtHostInterface::GetInstance()->pauseSystem(paused);
}

void DebuggerWindow::onRunToCursorTriggered()
{
  //
}

void DebuggerWindow::onGoToPCTriggered()
{
  // m_code_model->setSelection(CPU::g_state.regs.pc);
  scrollToPC();
}

void DebuggerWindow::onGoToAddressTriggered()
{
  //
}

void DebuggerWindow::onDumpAddressTriggered()
{
  //
}

void DebuggerWindow::onFollowAddressTriggered()
{
  //
}

void DebuggerWindow::onToggleBreakpointTriggered()
{
  std::optional<VirtualMemoryAddress> address = getSelectedCodeAddress();
  if (!address.has_value())
    return;

  toggleBreakpoint(address.value());
}

void DebuggerWindow::onStepIntoActionTriggered()
{
  Assert(System::IsPaused());
  m_registers_model->saveCurrentValues();
  QtHostInterface::GetInstance()->singleStepCPU();
  refreshAll();
}

void DebuggerWindow::onStepOverActionTriggered()
{
  Assert(System::IsPaused());
  if (!CPU::AddStepOverBreakpoint())
  {
    onStepIntoActionTriggered();
    return;
  }

  // unpause to let it run to the breakpoint
  m_registers_model->saveCurrentValues();
  QtHostInterface::GetInstance()->pauseSystem(false);
}

void DebuggerWindow::onStepOutActionTriggered()
{
  Assert(System::IsPaused());
  if (!CPU::AddStepOutBreakpoint())
  {
    QMessageBox::critical(this, tr("Debugger"), tr("Failed to add step-out breakpoint, are you in a valid function?"));
    return;
  }

  // unpause to let it run to the breakpoint
  m_registers_model->saveCurrentValues();
  QtHostInterface::GetInstance()->pauseSystem(false);
}

void DebuggerWindow::onCodeViewItemActivated(QModelIndex index)
{
  if (!index.isValid())
    return;

  const VirtualMemoryAddress address = m_code_model->getAddressForIndex(index);
  switch (index.column())
  {
    case 0: // breakpoint
    case 3: // disassembly
      toggleBreakpoint(address);
      break;

    case 1: // address
    case 2: // bytes
      scrollToMemoryAddress(address);
      break;

    case 4: // comment
      tryFollowLoadStore(address);
      break;
  }
}

void DebuggerWindow::setupAdditionalUi()
{
#ifdef WIN32
  QFont fixedFont;
  fixedFont.setFamily(QStringLiteral("Consolas"));
  fixedFont.setFixedPitch(true);
  fixedFont.setStyleHint(QFont::TypeWriter);
  fixedFont.setPointSize(10);
#else
  const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#endif
  m_ui.codeView->setFont(fixedFont);
  m_ui.registerView->setFont(fixedFont);
  m_ui.memoryView->setFont(fixedFont);
  m_ui.stackView->setFont(fixedFont);

  setCentralWidget(nullptr);
  delete m_ui.centralwidget;
}

void DebuggerWindow::connectSignals()
{
  QtHostInterface* hi = QtHostInterface::GetInstance();
  connect(hi, &QtHostInterface::emulationPaused, this, &DebuggerWindow::onEmulationPaused);
  connect(hi, &QtHostInterface::debuggerMessageReported, this, &DebuggerWindow::onDebuggerMessageReported);

  connect(m_ui.actionPause, &QAction::toggled, this, &DebuggerWindow::onPauseActionToggled);
  connect(m_ui.actionStepInto, &QAction::triggered, this, &DebuggerWindow::onStepIntoActionTriggered);
  connect(m_ui.actionStepOver, &QAction::triggered, this, &DebuggerWindow::onStepOverActionTriggered);
  connect(m_ui.actionStepOut, &QAction::triggered, this, &DebuggerWindow::onStepOutActionTriggered);
  connect(m_ui.actionToggleBreakpoint, &QAction::triggered, this, &DebuggerWindow::onToggleBreakpointTriggered);
  connect(m_ui.actionClose, &QAction::triggered, this, &DebuggerWindow::onCloseActionTriggered);

  connect(m_ui.codeView, &QTreeView::activated, this, &DebuggerWindow::onCodeViewItemActivated);
}

void DebuggerWindow::disconnectSignals()
{
  QtHostInterface* hi = QtHostInterface::GetInstance();
  hi->disconnect(this);
}

void DebuggerWindow::createModels()
{
  m_code_model = std::make_unique<DebuggerCodeModel>();
  m_ui.codeView->setModel(m_code_model.get());

  // set default column width in code view
  m_ui.codeView->setColumnWidth(0, 40);
  m_ui.codeView->setColumnWidth(1, 80);
  m_ui.codeView->setColumnWidth(2, 80);
  m_ui.codeView->setColumnWidth(3, 250);
  m_ui.codeView->setColumnWidth(4, m_ui.codeView->width() - (40 + 80 + 80 + 250));

  m_registers_model = std::make_unique<DebuggerRegistersModel>();
  m_ui.registerView->setModel(m_registers_model.get());
  // m_ui->registerView->resizeRowsToContents();

  m_stack_model = std::make_unique<DebuggerStackModel>();
  m_ui.stackView->setModel(m_stack_model.get());
}

void DebuggerWindow::setUIEnabled(bool enabled)
{
  // Disable all UI elements that depend on execution state
  m_ui.codeView->setEnabled(enabled);
  m_ui.registerView->setEnabled(enabled);
  m_ui.stackView->setEnabled(enabled);
  m_ui.memoryView->setEnabled(enabled);
  m_ui.actionRunToCursor->setEnabled(enabled);
  m_ui.actionToggleBreakpoint->setEnabled(enabled);
  m_ui.actionStepInto->setEnabled(enabled);
  m_ui.actionStepOver->setEnabled(enabled);
  m_ui.actionStepOut->setEnabled(enabled);
  m_ui.actionGoToAddress->setEnabled(enabled);
  m_ui.actionGoToPC->setEnabled(enabled);
}

void DebuggerWindow::setActiveMemoryRegion(ActiveMemoryRegion region)
{
  switch (region)
  {
    case ActiveMemoryRegion::RAM:
      m_ui.memoryView->setData(Bus::RAM_BASE, Bus::g_ram, Bus::RAM_SIZE);
      break;

    case ActiveMemoryRegion::Scratchpad:
      m_ui.memoryView->setData(CPU::DCACHE_LOCATION, CPU::g_state.dcache.data(), CPU::DCACHE_SIZE);
      break;

    case ActiveMemoryRegion::EXP1:
    default:
      m_ui.memoryView->setData(Bus::EXP1_BASE, nullptr, 0);
      break;
  }

  m_ui.memoryView->repaint();
}

void DebuggerWindow::toggleBreakpoint(VirtualMemoryAddress address)
{
  const bool new_bp_state = !CPU::HasBreakpointAtAddress(address);
  if (new_bp_state)
  {
    if (!CPU::AddBreakpoint(address, false))
      return;
  }
  else
  {
    if (!CPU::RemoveBreakpoint(address))
      return;
  }

  m_code_model->setBreakpointState(address, new_bp_state);
  refreshBreakpointList();
}

std::optional<VirtualMemoryAddress> DebuggerWindow::getSelectedCodeAddress()
{
  QItemSelectionModel* sel_model = m_ui.codeView->selectionModel();
  const QModelIndexList indices(sel_model->selectedIndexes());
  if (indices.empty())
    return std::nullopt;

  return m_code_model->getAddressForIndex(indices[0]);
}

bool DebuggerWindow::tryFollowLoadStore(VirtualMemoryAddress address)
{
  CPU::Instruction inst;
  if (!CPU::SafeReadInstruction(address, &inst.bits))
    return false;

  const std::optional<VirtualMemoryAddress> ea = GetLoadStoreEffectiveAddress(inst, &CPU::g_state.regs);
  if (!ea.has_value())
    return false;

  scrollToMemoryAddress(ea.value());
  return true;
}

void DebuggerWindow::scrollToMemoryAddress(VirtualMemoryAddress address)
{
  // TODO: Check region
  const PhysicalMemoryAddress phys_address = CPU::VirtualAddressToPhysical(address);
  m_ui.memoryView->scrollToAddress(phys_address);
}

void DebuggerWindow::refreshBreakpointList()
{
  while (m_ui.breakpointsWidget->topLevelItemCount() > 0)
    delete m_ui.breakpointsWidget->takeTopLevelItem(0);

  const CPU::BreakpointList bps(CPU::GetBreakpointList());
  for (const CPU::Breakpoint& bp : bps)
  {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, bp.enabled ? Qt::Checked : Qt::Unchecked);
    item->setText(0, QString::asprintf("%u", bp.number));
    item->setText(1, QString::asprintf("0x%08X", bp.address));
    item->setText(2, QString::asprintf("%u", bp.hit_count));
    m_ui.breakpointsWidget->addTopLevelItem(item);
  }
}

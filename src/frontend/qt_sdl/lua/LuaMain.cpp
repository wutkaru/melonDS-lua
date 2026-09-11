#include "lua/LuaMain.h"
#include <QDesktopServices>
#include <QFileDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include "ui_LuaConsoleDialog.h"

LuaBundle::LuaBundle(LuaConsoleDialog* dialog, EmuInstance* inst)
{
    emuInstance = inst;
    emuThread = emuInstance->getEmuThread();
    luaDialog = dialog;
    overlays = new std::vector<OverlayCanvas>;
    imageHash = new QHash<QString, QImage>;
    luaWidgets = new QList<QWidget*>;
}

LuaConsoleDialog::LuaConsoleDialog(QWidget* parent) : QDialog(parent)
{
    QWidget* w = parent;
    MainWindow* mainWindow;
    for (;;) //copied from ScreenPanel in Screen.cpp
    {
        mainWindow = qobject_cast<MainWindow*>(w);
        if (mainWindow) break;
        w = w->parentWidget();
        if (!w) break;
    }
    bundle = new LuaBundle(this,mainWindow->getEmuInstance());
    ui = new Ui::LuaConsoleDialog;
    ui->setupUi(this);
    console = ui->console;
    bar = console->verticalScrollBar();
    connect(ui->btnBrowse,&QPushButton::clicked,this,&LuaConsoleDialog::onOpenScript);
    connect(ui->btnRun,&QPushButton::clicked,this,&LuaConsoleDialog::onRunScript);
    connect(ui->btnEdit,&QPushButton::clicked,this,&LuaConsoleDialog::onEditScript);
    connect(ui->btnStop,&QPushButton::clicked,this,&LuaConsoleDialog::onStop);
    connect(ui->btnPause,&QPushButton::clicked,this,&LuaConsoleDialog::onPausePlay);
    connect(ui->btnClear,&QPushButton::clicked,console,&LuaConsole::onClear);
    refreshButtons();
}

LuaConsoleDialog::~LuaConsoleDialog()
{
    delete ui;
}

//Enables only the actions that make sense for the current script state.
void LuaConsoleDialog::refreshButtons()
{
    bool hasScript = currentScript.exists();
    bool running = bundle && bundle->getLuaState() != nullptr;
    lastRunning = running;
    ui->btnRun->setEnabled(hasScript);
    ui->btnEdit->setEnabled(hasScript);
    ui->btnStop->setEnabled(running);
    ui->btnPause->setEnabled(running);
    if (!running) bundle->flagPause = false;
    ui->btnPause->setText(bundle->flagPause ? "Resume" : "Pause");
}

//Points the dialog at a script and starts it, reporting what happened.
void LuaConsoleDialog::loadScript(const QFileInfo& file)
{
    currentScript = file;
    ui->txtScriptPath->setText(file.absoluteFilePath());
    setWindowTitle("Lua Script - " + file.fileName());
    bundle->printText("Loaded " + file.absoluteFilePath());
    bundle->flagPause = false;
    bundle->flagNewLua = true;
    refreshButtons();
}

void LuaConsoleDialog::closeEvent(QCloseEvent *event)
{
    onStop();
    bundle->overlays->clear();
    flagClosed = true;
    event->accept();
}

void LuaConsoleDialog::onOpenScript()
{
    //Reopen where the last script came from rather than always the cwd.
    QString startDir = currentScript.exists() ? currentScript.dir().path() : QDir::currentPath();
    QFileInfo file = QFileInfo(QFileDialog::getOpenFileName(this, "Load Lua Script",startDir,"Lua scripts (*.lua);;All files (*)"));
    if (!file.exists()) return;
    loadScript(file);
}

void LuaConsoleDialog::onRunScript()
{
    if (!currentScript.exists())
    {
        bundle->printText("No script loaded.");
        return;
    }
    //Re-reads the file from disk, so this doubles as a reload after editing.
    loadScript(currentScript);
}

void LuaConsoleDialog::onEditScript()
{
    if (!currentScript.exists()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(currentScript.absoluteFilePath()));
}

LuaConsole::LuaConsole(QWidget* parent)
{
    this->setParent(parent);
}

void LuaConsole::onGetText(const QString& string)
{
    this->appendPlainText(string);
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->maximum());
}

void LuaBundle::printText(QString string)
{
    this->luaDialog->console->onGetText(string);
}

void LuaConsole::onClear()
{
    this->clear();
}

LuaFunction::LuaFunction(luaFunctionPointer cf,const char* n,std::vector<LuaFunction*>* container)
{
    this->cfunction = cf;
    this->name = n;
    container->push_back(this);
}

static_assert(sizeof(LuaBundle*) <= LUA_EXTRASPACE,"LUA_EXTRASPACE too small");

LuaBundle* get_bundle(lua_State * L)
{
	LuaBundle* pBundle;
	std::memcpy(&pBundle, lua_getextraspace(L), sizeof(LuaBundle*));
	return pBundle;
}

#define MELON_LUA_HOOK_INSTRUCTION_COUNT 50 //number of vm instructions between hook calls
void luaHookFunction(lua_State* L, lua_Debug *arg)
{
    LuaBundle* bundle = get_bundle(L);
    if (bundle->flagStop and (arg->event == LUA_HOOKCOUNT)) 
        luaL_error(L, "Force Stopped");
}

std::vector<LuaFunction*> definedLuaFunctions;//List of all defined lua functions
QList<LuaLibrary*> luaLibraries;

LuaLibrary::LuaLibrary(const char* libName,std::vector<luaL_Reg>* luaFuncs)
{
    this->libName=libName;
    this->luaFuncs=luaFuncs;
    luaLibraries.push_back(this);
}

void LuaLibrary::load(lua_State* L)
{
    lua_createtable(L, 0, this->luaFuncs->size());
    this->luaFuncs->push_back((luaL_Reg){NULL,NULL});//append sentinel value
    luaL_setfuncs(L,this->luaFuncs->data(),0);
    this->luaFuncs->pop_back();
    lua_setglobal(L,this->libName);
}

void LuaBundle::createLuaState()
{
    if (!flagNewLua) return;
    overlays->clear();
    flagNewLua = false;
    emuInstance->setLuaInputMask(0xFFF);
    luaState = nullptr;
    QByteArray fileName = luaDialog->currentScript.fileName().toLocal8Bit();
    QString filedir = luaDialog->currentScript.dir().path();
    lua_State* L = luaL_newstate();
    LuaBundle* pBundle = this;
    std::memcpy(lua_getextraspace(L), &pBundle, sizeof(LuaBundle*)); //Write a pointer to this LuaBundle into the extra space of the new lua_State
    luaL_openlibs(L);
    for (LuaFunction* function : definedLuaFunctions)
        lua_register(L,function->name,function->cfunction);
    for (LuaLibrary* lLib: luaLibraries)
        lLib->load(L);
    QDir::setCurrent(filedir);
    lua_sethook(L,&luaHookFunction,LUA_MASKCOUNT,MELON_LUA_HOOK_INSTRUCTION_COUNT); 
    if (luaL_dofile(L,fileName.data())==LUA_OK)
    {
        luaState = L;
    }
    else //Error loading script
    {
        printText(lua_tostring(L,-1));
    }
}

void LuaConsoleDialog::onStop()
{
    bundle->getEmuInstance()->setLuaInputMask(0xFFF);
    if (bundle->getLuaState()) 
        bundle->flagStop = true;
}

void LuaConsoleDialog::onPausePlay()
{
    bundle->flagPause = !bundle->flagPause;
    ui->btnPause->setText(bundle->flagPause ? "Resume" : "Pause");
}

void LuaConsoleDialog::onLuaUpdate()
{
    bundle->createLuaState();
    bundle->luaUpdate();
    //A script can stop on its own (error, or Stop taking effect), so keep the
    //buttons honest without touching them every single frame.
    if ((bundle->getLuaState() != nullptr) != lastRunning)
        refreshButtons();
}

//Gets Called once a frame
void LuaBundle::luaUpdate()
{
    if (!luaState || flagPause) return;
    if (lua_getglobal(luaState,"_Update")!=LUA_TFUNCTION)
    {
        printText("No \"_Update\" Function found, pausing script...");
        flagPause = true;
        return;
    }
    if (lua_pcall(luaState,0,0,0)!=0)
    {
        //Handel Errors
        printText(lua_tostring(luaState,-1));
        emuInstance->setLuaInputMask(0xFFF);
        luaState = nullptr;
    }
}

//"Global" lua functions

namespace luaDefinitions
{

#define AddLuaFunction(functPointer,name)LuaFunction name(functPointer,#name,&definedLuaFunctions)

int lua_MelonPrint(lua_State* L)
{
    LuaBundle* bundle = get_bundle(L);
    const char* string = luaL_checkstring(L,1);
    bundle->printText((QString)string);
    return 0;
}
AddLuaFunction(lua_MelonPrint,print); //re-defines gloabal "print" function

}

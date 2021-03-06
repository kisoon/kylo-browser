/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2005-2012 Hillcrest Laboratories, Inc. All rights reserved.
 * Hillcrest Labs, the Loop, Kylo, the Kylo logo and the Kylo cursor are
 * trademarks of Hillcrest Laboratories, Inc.
 * */

#include "UDLRTool.h"
#include "nsXPCOM.h"
#include "nsServiceManagerUtils.h"
#include "nsIWindowMediator.h"
#include "nsIDocShell.h"
#include "nsIBaseWindow.h"
#include "nsIXULWindow.h"
#include "nsIWidget.h"
#include "nsEmbedString.h"
#include "nsIClassInfoImpl.h"
#include "nsMemory.h"
#include <stdio.h>
#include <fstream>

//#define _DEBUG_

#define TIMER 10
#define MIN_SPEED 3
#define MAX_SPEED 10

#define ACCELERATION 0.1

#define FULLSCREEN_FLASH_CLASS_NAME L"ShockwaveFlashFullScreen"
#define FULLSCREEN_SILVERLIGHT_CLASS_NAME L"AGFullScreenWinClass"
#define OOP_PLUGIN_CLASS_NAME L"GeckoPluginWindow"
#define FLASH_SANDBOX_CLASS_NAME L"GeckoFPSandboxChildWindow"

static HINSTANCE hinstDLL;
static HHOOK keyboardhhook;
static HHOOK shellhhook;
static UDLRTool* myself;
static HWND mainHWND;
static DWORD mainPID;

#ifdef _DEBUG_
    std::wofstream _log;
#endif

//NS_IMPL_ISUPPORTS1(UDLRTool, IUDLRTool)
NS_IMPL_CLASSINFO(UDLRTool, NULL, 0, UDLRTOOL_CID)
NS_IMPL_ISUPPORTS1_CI(UDLRTool, IUDLRTool)

UDLRTool::UDLRTool()
{
    // Set up defaults
    upKey_ = VK_UP;
    downKey_ = VK_DOWN;
    leftKey_ = VK_LEFT;
    rightKey_ = VK_RIGHT;

    upKeyState_ = false;
    downKeyState_ = false;
    leftKeyState_ = false;
    rightKeyState_ = false;

    minSpeed_ = MIN_SPEED;
    maxSpeed_ = MAX_SPEED;
    acceleration_ = ACCELERATION;
    speed_ = 0;

    pressed_ = false;
    capture_ = true;
    
    leftClickKey_ = VK_RETURN;
    rightClickKey_ = NULL;
    middleClickKey_ = NULL;
    scrollUpKey_ = NULL;
    scrollDownKey_ = NULL;
    
    /**
     * We're going to get the main Kylo window through some special XPCOM APIs.
     *
     * Steps:
     * 1) Get the WindowMediator service
     * 2) Get an enumerator of all XUL window objects with a "contenttype" of "poloContent"
     * 3) Take the first window. It should be the *only* window because Kylo is
     *    designed to only have 1 main window
     */
    nsCOMPtr<nsIServiceManager> svcMgr;
    nsresult rv = NS_GetServiceManager(getter_AddRefs(svcMgr));

    if (NS_FAILED(rv))
    {
#ifdef _DEBUG_
        _log << "Can't get the service manager!" << std::endl;
#endif
        NS_ShutdownXPCOM(nsnull);
        return;
    }

    nsCOMPtr<nsIWindowMediator> winMediator;
    rv = svcMgr->GetServiceByContractID(NS_WINDOWMEDIATOR_CONTRACTID,
            NS_GET_IID(nsIWindowMediator), getter_AddRefs(winMediator));

    if (NS_FAILED(rv))
    {
#ifdef _DEBUG_
        _log << "Can't get the WindowMediator service!" << std::endl;
#endif
        return;
    }

    // Get the enumerator
    nsCOMPtr<nsISimpleEnumerator> winEnum;
    winMediator->GetXULWindowEnumerator(NS_LITERAL_STRING("poloContent").get(), getter_AddRefs(winEnum));

    bool more;
    winEnum->HasMoreElements(&more);
    if (!more) {
#ifdef _DEBUG_
        _log << "No XUL windows!" << std::endl;
#endif
        return;
    }

    // Get the first item in the enumerator
    nsCOMPtr<nsIXULWindow> xulWin;
    winEnum->GetNext(getter_AddRefs(xulWin));

    // Get the docShell from the window...
    nsCOMPtr<nsIDocShell> docShell;
    xulWin->GetDocShell(getter_AddRefs(docShell));

    // ...which becomes nsIBaseWindow...
    nsIBaseWindow* win = 0;
    docShell->QueryInterface(NS_GET_IID(nsIBaseWindow), (void**)&win);

    // ...which lets me get the "main widget"...
    nsCOMPtr<nsIWidget> widget;
    win->GetMainWidget(getter_AddRefs(widget));

    // ...which gives me the native window handle...
    mainHWND = (HWND) widget->GetNativeData(NS_NATIVE_WINDOW);

    // ...and now I can get the PID (for comparison).
    GetWindowThreadProcessId(mainHWND, &mainPID);

    keyboardhhook = NULL;
    
#ifdef _DEBUG_
    WCHAR buff[512];
    _snwprintf_s(buff, _countof(buff), _TRUNCATE, L"%s\\Hillcrest Labs\\Kylo\\udlrtool.log", _wgetenv(L"APPDATA"));
    _log.open(buff, std::ios::app);
    _log << "starting UDLRTool" << std::endl;
#endif
}

UDLRTool::~UDLRTool()
{
    /* destructor code */
    if (keyboardhhook != NULL)
    {
        RemoveHook();
    }

    #ifdef _DEBUG_
        _log << "ending UDLRTool" << std::endl;
    #endif
}

HINSTANCE GetHInstance()
{
    MEMORY_BASIC_INFORMATION mbi;
    CHAR szModule[MAX_PATH];

    SetLastError(ERROR_SUCCESS);
    if (VirtualQuery(GetHInstance,&mbi,sizeof(mbi)))
    {
        if (GetModuleFileName((HINSTANCE)mbi.AllocationBase, (LPWCH) szModule,sizeof(szModule)))
        {
            return (HINSTANCE)mbi.AllocationBase;
        }        
    }
    return NULL;
}

bool IsFGWinKylo()
{
    HWND fgWin = GetForegroundWindow();
    DWORD fgPID;
    GetWindowThreadProcessId(fgWin, &fgPID);

    WCHAR fgWinClassName[512];

    GetClassName(fgWin, fgWinClassName, 512);

    bool fullScreenPlugin = false;
    bool oopPlugin = false;
    bool flashSB = false;

    fullScreenPlugin = (lstrcmpi(fgWinClassName, FULLSCREEN_FLASH_CLASS_NAME) == 0 ||
                        lstrcmpi(fgWinClassName, FULLSCREEN_SILVERLIGHT_CLASS_NAME) == 0);

    oopPlugin = (lstrcmpi(fgWinClassName, OOP_PLUGIN_CLASS_NAME) == 0);

    flashSB = (lstrcmpi(fgWinClassName, FLASH_SANDBOX_CLASS_NAME) == 0);

    return (fgPID == mainPID || fullScreenPlugin || oopPlugin || flashSB);
}

void UDLRTool::AddHook()
{
    if (keyboardhhook != NULL) {
        // Don't add hook if it's already enabled
        return;
    }
    myself = this;

    hinstDLL = GetHInstance();

    uIDEvent_ = SetTimer(0, 0, TIMER, TimerProc);

    keyboardhhook = SetWindowsHookEx(WH_KEYBOARD_LL,
        KeyboardHookProc,
        hinstDLL,
        0);

    return;
}

void UDLRTool::RemoveHook() 
{
    bool result = KillTimer(NULL, uIDEvent_);
    if (keyboardhhook != NULL) {
        UnhookWindowsHookEx(keyboardhhook);
        keyboardhhook = NULL;
    }
}

VOID CALLBACK UDLRTool::TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    if (IsFGWinKylo()) {
        myself->HandleTimerEvent();
    }
}

void UDLRTool::HandleTimerEvent() {
    DWORD dwFlags = 0;

    int dx = 0;
    int dy = 0;
    if (downKeyState_ || upKeyState_ || leftKeyState_ || rightKeyState_) {
        dwFlags |= MOUSEEVENTF_MOVE;
        if (pressed_) {
            speed_ += acceleration_;
            if (speed_ > maxSpeed_) {
                speed_ = maxSpeed_;
            }
        } else {
            speed_ = minSpeed_;
        }
        pressed_ = true;
    } else {
        pressed_ = false;
        speed_ = 0;
        return;
    }

    if (downKeyState_) {
        dy = speed_;
    }

    if (upKeyState_) {
        dy -= speed_;
    }

    if (rightKeyState_) {
        dx += speed_;
    }

    if (leftKeyState_) {
        dx -= speed_;
    }

#ifdef _DEBUG_
    _log << "Speed: " << dx << ", " << dy << std::endl;
#endif

    mouse_event(dwFlags, dx, dy, 0, 0);
}

LRESULT WINAPI UDLRTool::KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == 0) {
        if (myself->HandleKeyEvent(wParam, lParam)) {
            return 1;
        }
    }

    return CallNextHookEx(keyboardhhook, nCode, wParam, lParam);
}

bool UDLRTool::HandleKeyEvent(WPARAM wParam, LPARAM lParam) {

    if (!IsFGWinKylo()) {
        return false;
    }

    bool buttonPress = (wParam == WM_KEYDOWN);
    bool capture = false;

    PRInt16 vkCode = ((KBDLLHOOKSTRUCT *) lParam)->vkCode;

#ifdef _DEBUG_
    _log << "vkCode: " << vkCode << std::endl;
#endif

    if (!vkCode) {
        return false;
    }

    if (vkCode == upKey_) {
        upKeyState_ = buttonPress;
    } else if (vkCode == downKey_) {
        downKeyState_ = buttonPress;
    } else if (vkCode == leftKey_) {
        leftKeyState_ = buttonPress;
    } else if (vkCode == rightKey_) {
        rightKeyState_ = buttonPress;
    }

    // Handle arrow keys
    if (capture_ &&
            (vkCode == upKey_ ||
             vkCode == downKey_ ||
             vkCode == leftKey_ ||
             vkCode == rightKey_)) {
        capture = capture_;
    }

    // Handle mouse events
    if (buttonPress) {
        if (vkCode == leftClickKey_) {
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            capture = capture_;
        }
        if (vkCode == rightClickKey_) {
            mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            capture = capture_;
        }
        if (vkCode == middleClickKey_) {
            mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0);
            capture = capture_;
        }
        if (vkCode == scrollUpKey_) {
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, WHEEL_DELTA, 0);
            capture = capture_;
        }
        if (vkCode == scrollDownKey_) {
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, -WHEEL_DELTA, 0);
            capture = capture_;
        }
    }

    return capture;
}


/* void enableUDLR (); */
NS_IMETHODIMP UDLRTool::EnableUDLR()
{
#ifdef _DEBUG_;
    _log << "enabling" << std::endl;
#endif
    AddHook();
    return NS_OK;
}

/* void disableUDLR (); */
NS_IMETHODIMP UDLRTool::DisableUDLR()
{
#ifdef _DEBUG_;
    _log << "disabling" << std::endl;
#endif
    RemoveHook();
    return NS_OK;
}

/* void captureKeypresses (in boolean shouldCapture); */

#ifdef XPCOM_USE_PRBOOL
NS_IMETHODIMP UDLRTool::CaptureKeypresses(PRBool shouldCapture)
{
#else
NS_IMETHODIMP UDLRTool::CaptureKeypresses(bool shouldCapture)
{
#endif
    capture_ = shouldCapture;
    return NS_OK;
}

/* void setUDLRKeys (in short upKey, in short downKey, in short leftKey, in short rightKey); */
NS_IMETHODIMP UDLRTool::SetUDLRKeys(PRInt16 upKey, PRInt16 downKey, PRInt16 leftKey, PRInt16 rightKey)
{
    upKey_ = upKey;
    downKey_ = downKey;
    leftKey_ = leftKey;
    rightKey_ = rightKey;
    return NS_OK;
}

/* void setButtonKeys (in short leftClickKey, in short rightClickKey, in short middleClickKey, in short scrollUpKey, in short scrollDownKey); */
NS_IMETHODIMP UDLRTool::SetButtonKeys(PRInt16 leftClickKey, PRInt16 rightClickKey, PRInt16 middleClickKey, PRInt16 scrollUpKey, PRInt16 scrollDownKey)
{
    leftClickKey_ = leftClickKey;
    rightClickKey_ = rightClickKey;
    middleClickKey_ = middleClickKey;
    scrollUpKey_ = scrollUpKey;
    scrollDownKey_ = scrollDownKey;
    return NS_OK;
}

/* void setSpeed (in short min, in short max); */
NS_IMETHODIMP UDLRTool::SetSpeed(PRInt16 min, PRInt16 max)
{
    minSpeed_ = min;
    maxSpeed_ = max;
    return NS_OK;
}

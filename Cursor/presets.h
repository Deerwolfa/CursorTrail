#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <deque>
#include <vector>
#include <map>
#include <memory>
#include <cmath>
#include <cstring>
#include <cstdio>

struct Preset { const wchar_t* name; Mode mode; int count; COLORREF cols[MAX_COLORS]; };

static const Preset kPresets[] = {
    { L"Neon",     MODE_GRADIENT, 3,{ RGB(0,255,200), RGB(120,80,255), RGB(255,0,220) } },
    { L"Fire",     MODE_GRADIENT, 5,{ RGB(255,255,200), RGB(255,230,60), RGB(255,140,0), RGB(220,20,0), RGB(80,0,0) } },
    { L"Ocean",    MODE_GRADIENT, 4,{ RGB(170,255,255), RGB(0,200,255), RGB(0,90,220), RGB(0,20,120) } },
    { L"Sunset",   MODE_GRADIENT, 5,{ RGB(255,220,90), RGB(255,130,60), RGB(240,60,120), RGB(150,0,190), RGB(40,0,110) } },
    { L"Forest",   MODE_GRADIENT, 4,{ RGB(210,255,100), RGB(100,220,60), RGB(0,140,70), RGB(0,50,40) } },
    { L"Spectrum (10 colors)", MODE_GRADIENT, 10,
        { RGB(255,0,0), RGB(255,128,0), RGB(255,230,0), RGB(150,255,0), RGB(0,220,60),
          RGB(0,230,230), RGB(0,130,255), RGB(40,40,255), RGB(150,0,255), RGB(255,0,200) } },
    { L"Rainbow (animated)", MODE_RAINBOW, 2,{ RGB(255,0,0), RGB(255,0,0) } },
    { L"White",    MODE_SOLID,    1,{ RGB(255,255,255) } },
};
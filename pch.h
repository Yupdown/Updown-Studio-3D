#pragma once

#define DIRECTINPUT_VERSION 0x0800

// C++ Standard Library
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Windows Library
#include <Windows.h>
#include <windowsx.h>
#include <wrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <comdef.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wincodecsdk.h>

// Directx12 Library
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dinput.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include <DirectXPackedVector.h>
#include <DirectXCollision.h>
#include <DirectXColors.h>

// Third Party Library

// In-Engine Library
#include "SimpleMath.h"
#include "define.h"
#include "custom_math.h"

// Link Static Library
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
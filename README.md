# Remote Server TaskTray

Windows用のタスクトレイアプリケーションで、GPU情報を管理し、ハードウェアエンコーディングをサポートします。

## 必要な環境

- Windows 10/11
- CMake 3.16以上
- Visual Studio 2019以上またはMinGW-w64
- DirectX SDK（Windows SDKに含まれる）

## CMakeでのビルド方法

### 1. ビルドディレクトリの作成

```powershell
mkdir build
cd build
```

### 2. CMakeの設定

#### Visual Studio使用の場合:
```powershell
cmake .. -G "Visual Studio 17 2022" -A x64
```

#### MinGW使用の場合:
```powershell
cmake .. -G "MinGW Makefiles"
```

### 3. ビルド実行

#### Debugビルド:
```powershell
cmake --build . --config Debug
```

#### Releaseビルド:
```powershell
cmake --build . --config Release
```

### 4. 実行ファイルの場所

ビルド完了後、実行ファイルは以下の場所に生成されます：
- Debug版: `build/Debug/remote_server_tasktray.exe`
- Release版: `build/Release/remote_server_tasktray.exe`

## 従来のVisual Studioでのビルド方法

従来通りVisual Studioを使用する場合：

1. `remote_server_tasktray.sln` をVisual Studioで開く
2. ビルド設定を選択（Debug/Release, x64）
3. ビルド → ソリューションのビルド

## プロジェクト構成

- **remote_server_tasktray.cpp**: メインエントリーポイント
- **TaskTrayApp**: タスクトレイアプリケーションの管理
- **GPUManager**: GPU情報の取得とハードウェアエンコーディング対応チェック
- **RegistryHelper**: レジストリ操作
- **SharedMemoryHelper**: 共有メモリ操作
- **DebugLog**: デバッグログ出力

## 機能

- システムに搭載されたGPUの検出
- ハードウェアエンコーディング対応チェック
- GPU情報のレジストリと共有メモリへの保存
- タスクトレイでの常駐動作

## 依存ライブラリ

- DXGI (DirectX Graphics Infrastructure)
- Direct3D 11
- Windows API (User32, Kernel32, Advapi32 など)

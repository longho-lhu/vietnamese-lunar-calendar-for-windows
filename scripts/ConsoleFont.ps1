#Requires -Version 5.1
# Phông mặc định của cửa sổ console cũ (Lucida Console, phông raster) thiếu các
# chữ có dấu riêng của tiếng Việt (ạ, ừ, ế, ở...) nên chúng hiện thành ô trống.
# Script này đổi cửa sổ hiện tại sang Consolas (có đủ dấu), giữ nguyên cỡ chữ.
# Dùng: . (Join-Path $PSScriptRoot 'ConsoleFont.ps1')

function Set-VietnameseConsoleFont {
    if ($Host.Name -ne 'ConsoleHost') { return }
    try {
        if (-not ('AmLich.ConsoleFont' -as [type])) {
            Add-Type -Namespace AmLich -Name ConsoleFont -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct FontInfo {
    public uint cbSize;
    public uint nFont;
    public short X;
    public short Y;
    public int FontFamily;
    public int FontWeight;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FaceName;
}
[DllImport("kernel32.dll", SetLastError = true)]
public static extern IntPtr GetStdHandle(int n);
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool GetCurrentConsoleFontEx(IntPtr h, bool max, ref FontInfo info);
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool SetCurrentConsoleFontEx(IntPtr h, bool max, ref FontInfo info);
'@
        }
        $handle = [AmLich.ConsoleFont]::GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        $info = New-Object AmLich.ConsoleFont+FontInfo
        $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf([type][AmLich.ConsoleFont+FontInfo])
        if (-not [AmLich.ConsoleFont]::GetCurrentConsoleFontEx($handle, $false, [ref]$info)) { return }
        if ($info.FaceName -in 'Consolas', 'Cascadia Mono', 'Cascadia Code') { return }
        $info.FaceName = 'Consolas'
        $info.FontFamily = 54   # TMPF_TRUETYPE | FF_MODERN
        $info.FontWeight = 400
        $info.X = 0
        if ($info.Y -lt 14) { $info.Y = 16 }
        [AmLich.ConsoleFont]::SetCurrentConsoleFontEx($handle, $false, [ref]$info) | Out-Null
    } catch { }
}

Set-VietnameseConsoleFont

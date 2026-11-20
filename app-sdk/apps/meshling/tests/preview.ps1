# Run from the repository root after building bench/build/lssim.exe.
# Captures real simulator frames; System.Drawing only arranges them as sheets.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = $PWD.Path
$out = Join-Path $root 'bench/build/meshling-out'
New-Item -ItemType Directory -Force $out | Out-Null
$forms = @('egg','hatchling','sprout','relay','swarm','scanner','nexus','drifter','relic')
$font = [System.Drawing.Font]::new('Consolas',14)
$brush = [System.Drawing.Brushes]::White
$overview = [System.Drawing.Bitmap]::new(1200,1650)
$og = [System.Drawing.Graphics]::FromImage($overview)
$og.Clear([System.Drawing.Color]::Black)
$og.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$og.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
Push-Location (Join-Path $root 'bench/build')
try {
    $env:LAPSIM = 'meshling'
    Remove-Item Env:LSSIM_FRAMES -ErrorAction SilentlyContinue
    Remove-Item Env:LAPSIM_PROFILE -ErrorAction SilentlyContinue
    for ($i=0; $i -lt $forms.Count; $i++) {
        $form = $forms[$i]
        $sheet = [System.Drawing.Bitmap]::new(1136,1636)
        $g = [System.Drawing.Graphics]::FromImage($sheet)
        $g.Clear([System.Drawing.Color]::Black)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $g.DrawString($form.ToUpper() + ' / METAL + BAYER', $font, $brush, 16, 8)
        for ($mode=0; $mode -lt 6; $mode++) {
            $mood = @('day','night','happy','hungry','sleep','chomp')[$mode]
            $hour = if ($mode -eq 1) {23} else {12}
            $extra = if ($mode -ge 2) {";79:pet=$mood"} else {''}
            $env:LAPSIM_SCRIPT = "0:pet=$form;0:hour=$hour;78:pet=$form$extra"
            $bmp = Join-Path $out "$form-$mood.bmp"
            $count = if ($mode -eq 1) {111} else {81}
            & ./lssim.exe lap -A 40 -f $count -o $bmp | Out-Null
            if ($LASTEXITCODE) { throw "Simulator failed: $form/$mood" }
            $im = [System.Drawing.Image]::FromFile($bmp)
            if ($mode -lt 2) {
                $g.DrawImageUnscaled($im, ($mode*568), 40)
                if ($mode -eq 0) {
                    $ox=($i%3)*400; $oy=[int][Math]::Floor($i/3)*550
                    $og.DrawString($form.ToUpper(), $font, $brush, $ox+12, $oy+8)
                    $og.DrawImage($im,[System.Drawing.Rectangle]::new($ox,$oy+38,400,500),[System.Drawing.Rectangle]::new(88,420,400,500),[System.Drawing.GraphicsUnit]::Pixel)
                }
            } else {
                $x=($mode-2)*284
                $g.DrawString($mood.ToUpper(), $font, $brush, $x+8, 1284)
                $g.DrawImage($im,[System.Drawing.Rectangle]::new($x,1316,284,310),[System.Drawing.Rectangle]::new(98,482,380,416),[System.Drawing.GraphicsUnit]::Pixel)
            }
            $im.Dispose()
            Remove-Item -LiteralPath $bmp
        }
        $g.Dispose()
        $sheet.Save((Join-Path $out "$form.png"),[System.Drawing.Imaging.ImageFormat]::Png)
        $sheet.Dispose()
        Write-Output "Captured $form"
    }
    # An actual change of form at frame 80, before/during/after the scan.
    $strip=[System.Drawing.Bitmap]::new(1500,460)
    $sg=[System.Drawing.Graphics]::FromImage($strip)
    $sg.Clear([System.Drawing.Color]::Black)
    $sg.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $sg.PixelOffsetMode=[System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $frames=@(79,83,92,100,112)
    for($i=0;$i -lt $frames.Count;$i++) {
        $env:LAPSIM_SCRIPT='0:pet=sprout;80:pet=scanner'
        $bmp=Join-Path $out 'evolution-temp.bmp'
        & ./lssim.exe lap -A 40 -f $frames[$i] -o $bmp | Out-Null
        if($LASTEXITCODE) {throw 'Evolution capture failed'}
        $im=[System.Drawing.Image]::FromFile($bmp)
        $sg.DrawString(@('SPROUT','SCAN / 80 ms','SCAN / 440 ms','SCAN / 760 ms','SCANNER')[$i],$font,$brush,$i*300+10,10)
        $sg.DrawImage($im,[System.Drawing.Rectangle]::new($i*300,42,300,408),[System.Drawing.Rectangle]::new(124,418,340,462),[System.Drawing.GraphicsUnit]::Pixel)
        $im.Dispose(); Remove-Item -LiteralPath $bmp
    }
    $sg.Dispose(); $strip.Save((Join-Path $out 'evolution.png'),[System.Drawing.Imaging.ImageFormat]::Png); $strip.Dispose()
} finally {
    Pop-Location
    $og.Dispose()
    $overview.Save((Join-Path $out 'all-forms.png'),[System.Drawing.Imaging.ImageFormat]::Png)
    $overview.Dispose(); $font.Dispose()
}

param([Parameter(Mandatory)][string]$Source,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$image=[Drawing.Image]::FromFile([IO.Path]::GetFullPath($Source))
$frames=@()
try {
    foreach($size in @(16,32,48,64,128,256)) {
        $bitmap=[Drawing.Bitmap]::new($size,$size)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        $stream=[IO.MemoryStream]::new()
        try { $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic; $graphics.DrawImage($image,0,0,$size,$size); $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png); $frames+=,@{size=$size;bytes=$stream.ToArray()} }
        finally {$stream.Dispose();$graphics.Dispose();$bitmap.Dispose()}
    }
    $writer=[IO.BinaryWriter]::new([IO.File]::Create([IO.Path]::GetFullPath($Output)))
    try {
        $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$frames.Count)
        $offset=6+16*$frames.Count
        foreach($frame in $frames){$dimension=if($frame.size -eq 256){0}else{$frame.size};$writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0);$writer.Write([uint16]1);$writer.Write([uint16]32);$writer.Write([uint32]$frame.bytes.Length);$writer.Write([uint32]$offset);$offset+=$frame.bytes.Length}
        foreach($frame in $frames){$writer.Write([byte[]]$frame.bytes)}
    }finally{$writer.Dispose()}
}finally{$image.Dispose()}

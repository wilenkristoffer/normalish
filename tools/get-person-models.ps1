# Fetches the two person segmentation models normalish uses to find the
# subject in the live webcam feed.
#
# Both are MediaPipe selfie segmentation models (Apache-2.0) converted to ONNX
# by others. Not committed to the repository, for the same reason as the depth
# model. Without them depth mode still works, but falls back to guessing the
# subject from the depth map alone, which is a nearest-object selector and
# not a person detector.

$ErrorActionPreference = 'Stop'

$dir = 'C:\ProgramData\normalish'
$models = @(
    @{ Name = 'selfie-segmentation-landscape.onnx'; Bytes = 462338
       Url = 'https://huggingface.co/onnx-community/mediapipe_selfie_segmentation_landscape/resolve/main/onnx/model.onnx' },
    @{ Name = 'selfie-multiclass.onnx'; Bytes = 16454497
       Url = 'https://huggingface.co/FreeHugsForRobots/ps-skin-seg/resolve/main/selfie_multiclass_256x256.onnx' }
)

if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir | Out-Null
    Write-Output "created $dir"
}

# Invoke-WebRequest's progress bar makes a large download several times
# slower in some hosts, so it is turned off for the duration.
$previous = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'
try {
    foreach ($m in $models) {
        $dest = Join-Path $dir $m.Name
        if (Test-Path $dest) {
            $have = (Get-Item $dest).Length
            if ($have -eq $m.Bytes) {
                Write-Output "already present: $dest"
                continue
            }
            Write-Output "size mismatch ($have bytes, expected $($m.Bytes)) - refetching"
        }

        Write-Output "downloading $($m.Name) ($($m.Bytes) bytes) from huggingface.co ..."
        Invoke-WebRequest -Uri $m.Url -OutFile $dest -UseBasicParsing

        $got = (Get-Item $dest).Length
        if ($got -ne $m.Bytes) {
            Remove-Item $dest
            throw "downloaded $got bytes, expected $($m.Bytes) - removed the partial file"
        }
        Write-Output "done: $dest"
    }
} finally {
    $ProgressPreference = $previous
}

Write-Output "normalish will pick them up on the next start."

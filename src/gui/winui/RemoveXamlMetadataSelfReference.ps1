param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$content = [System.IO.File]::ReadAllText($Path)
$pattern = '(?m)^\s*_otherProviders\.push_back\(::winrt::[A-Za-z0-9_]+::XamlMetaDataProvider\(\)\);\r?\n'
$matches = [System.Text.RegularExpressions.Regex]::Matches($content, $pattern)
if ($matches.Count -gt 1) {
    throw "Expected at most one self XAML metadata provider reference in '$Path', found $($matches.Count)."
}

if ($matches.Count -eq 1) {
    $updated = [System.Text.RegularExpressions.Regex]::Replace($content, $pattern, '', 1)
    [System.IO.File]::WriteAllText($Path, $updated, [System.Text.UTF8Encoding]::new($false))
}

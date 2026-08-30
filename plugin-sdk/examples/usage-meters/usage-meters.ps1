param([Parameter(ValueFromRemainingArguments = $true)][string[]]$RemainingArgs)

$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

function Send-Activity {
    param(
        [string]$Id,
        [string]$Title,
        [int]$Percent,
        [string]$Accent,
        [string]$Glyph,
        [int]$Priority
    )

    $message = @{
        type = 'activity.upsert'
        activity = @{
            id = $Id
            kind = 'metric'
            title = $Title
            subtitle = 'Usage'
            glyph = $Glyph
            accent = $Accent
            progress = [math]::Max(0, [math]::Min(1, $Percent / 100.0))
            value = $Percent
            valueSuffix = '%'
            priority = $Priority
            pinned = $true
            actions = @(
                @{ id = 'refresh'; label = 'Refresh'; glyph = '↻' }
            )
        }
    }
    [Console]::Out.WriteLine(($message | ConvertTo-Json -Compress -Depth 6))
    [Console]::Out.Flush()
}

function Publish-All {
    # Replace these values with real quota/API data in your own plugin.
    Send-Activity -Id 'agent-a' -Title 'Agent A' -Percent 73 -Accent '#FF5A1F' -Glyph '✦' -Priority 230
    Send-Activity -Id 'agent-b' -Title 'Agent B' -Percent 21 -Accent '#00E89A' -Glyph '◉' -Priority 220
    Send-Activity -Id 'agent-c' -Title 'Agent C' -Percent 52 -Accent '#E8FF19' -Glyph '✧' -Priority 210
}

Publish-All

while (($line = [Console]::In.ReadLine()) -ne $null) {
    try {
        $message = $line | ConvertFrom-Json
        if ($message.type -eq 'action.invoke' -and $message.actionId -eq 'refresh') {
            Publish-All
        }
    } catch {
        # Protocol errors are ignored by example plugins rather than writing logs to stdout,
        # because stdout is reserved for NDJSON protocol messages.
    }
}

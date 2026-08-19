# sync.config.example.ps1  --  template
#
# Copy this file to  sync.config.ps1  (which is gitignored) and fill in your
# values. run_sync.ps1 dot-sources sync.config.ps1 before launching the sync,
# so your credentials stay in this local file and never get committed.

$env:TICKER_IP   = "10.0.0.181"                  # the Matrix Portal's IP address
$env:TICKER_USER = "joe"                         # web-UI username
$env:TICKER_PASS = "your_web_ui_password"        # web-UI password (from arduino_secrets.h)

$env:APPLE_ID           = "you@icloud.com"       # your Apple ID / iCloud email
$env:APPLE_APP_PASSWORD = "xxxx-xxxx-xxxx-xxxx"  # app-specific password (appleid.apple.com)

# Optional: only poll these calendars (comma-separated). The sync logs the
# calendar names it finds at startup. Fewer calendars = fewer CalDAV requests
# per minute = less chance of hitting Apple's rate limit. Omit to poll all.
# $env:CALENDARS = "Calendar,Work,Home"

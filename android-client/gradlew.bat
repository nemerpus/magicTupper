@echo off
setlocal EnableExtensions
set "GRADLE_VERSION=8.9"
set "CACHE_DIR=%USERPROFILE%\.gradle\magicTupper-bootstrap"
set "GRADLE_HOME=%CACHE_DIR%\gradle-%GRADLE_VERSION%"
set "ZIP_FILE=%CACHE_DIR%\gradle-%GRADLE_VERSION%-bin.zip"

if not defined JAVA_HOME (
  if exist "%ProgramFiles%\Android\Android Studio\jbr\bin\java.exe" set "JAVA_HOME=%ProgramFiles%\Android\Android Studio\jbr"
)
if not defined JAVA_HOME (
  where java >nul 2>nul || (
    echo ERROR: No se encontro Java. Abre Android Studio una vez o define JAVA_HOME.
    exit /b 1
  )
)

if not exist "%GRADLE_HOME%\bin\gradle.bat" (
  echo [magicTupper] Preparando Gradle %GRADLE_VERSION% por primera vez...
  if not exist "%CACHE_DIR%" mkdir "%CACHE_DIR%"
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $u='https://services.gradle.org/distributions/gradle-%GRADLE_VERSION%-bin.zip'; $z='%ZIP_FILE%'; if (!(Test-Path $z)) { Invoke-WebRequest -Uri $u -OutFile $z -UseBasicParsing }; Expand-Archive -Path $z -DestinationPath '%CACHE_DIR%' -Force"
  if errorlevel 1 exit /b 1
)

call "%GRADLE_HOME%\bin\gradle.bat" %*
exit /b %ERRORLEVEL%

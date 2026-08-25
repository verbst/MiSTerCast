using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;

namespace MiSTerCast
{
    struct Modeline
    {
        public string name;
        public Double pclock;
        public UInt16 hactive;
        public UInt16 hbegin;
        public UInt16 hend;
        public UInt16 htotal;
        public UInt16 vactive;
        public UInt16 vbegin;
        public UInt16 vend;
        public UInt16 vtotal;
        public bool interlace;
    }

    struct SourceOptions
    {
        public byte display;
        public bool audio;
        public bool preview;
        public byte alignment;
        public byte cropmode;
        public UInt16 width;
        public UInt16 height;
        public Int16 xoffset;
        public Int16 yoffset;
        public byte rotation;
        public byte sampling;
    }

    sealed class WindowCaptureSource
    {
        public IntPtr Handle { get; set; }
        public string Title { get; set; }
        public string ProcessName { get; set; }

        public string Key
        {
            get { return ProcessName + "\t" + Title; }
        }

        public override string ToString()
        {
            return String.IsNullOrWhiteSpace(ProcessName)
                ? Title
                : Title + " — " + ProcessName;
        }
    }

    public partial class MainWindow : Window
    {
        private bool isInitialized = false;
        private bool isStreaming = false;
        HelpWindow helpWindow = null;
        const string lastSaveFilename = "lastsave.dat";
        string currentSaveFilename = null;
        private StreamWriter diagnosticLogWriter;
        private string diagnosticLogPath;
        private TextBlock telemetryLogText;
        private bool isRefreshingWindowSources;
        private bool isLoadingCaptureSettings;

        private delegate bool EnumWindowsCallback(IntPtr windowHandle, IntPtr parameter);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool IsWindowVisible(IntPtr windowHandle);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern int GetWindowText(IntPtr windowHandle, StringBuilder text, int maximumCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern int GetWindowTextLength(IntPtr windowHandle);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint GetWindowThreadProcessId(IntPtr windowHandle, out uint processId);

        [DllImport("user32.dll", EntryPoint = "GetWindowLongW", SetLastError = true)]
        private static extern int GetWindowLong(IntPtr windowHandle, int index);

        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr windowHandle, uint command);

        [DllImport("dwmapi.dll")]
        private static extern int DwmGetWindowAttribute(
            IntPtr windowHandle,
            int attribute,
            out int value,
            int valueSize);

        private const int ExtendedWindowStyleIndex = -20;
        private const int ToolWindowStyle = 0x00000080;
        private const int AppWindowStyle = 0x00040000;
        private const uint GetOwnerWindow = 4;
        private const int DwmWindowAttributeCloaked = 14;

        private void InitializeMiSTerCast()
        {
            if (LogDelegate == null)
                LogDelegate = new MiSTerCastInterop.LogDelegate(LogCallback);
            if (CaptureImageDelegate == null)
                CaptureImageDelegate = new MiSTerCastInterop.CaptureImageDelegate(CaptureImage);
            isInitialized = MiSTerCastInterop.Initialize(LogDelegate, CaptureImageDelegate);
            if (isInitialized)
            {
                // Push the stream options first: the modeline's byte budget
                // depends on the RGB mode, so validation needs them in place.
                // OnStreamOptionsChanged calls OnModelineChanged internally.
                OnStreamOptionsChanged();
            }
        }

        public MainWindow()
        {
            InitializeComponent();
            string diagnosticLogError = InitializeDiagnosticLog();
            if (diagnosticLogError == null)
                Log("Diagnostic log: " + diagnosticLogPath);
            else
                Log("Creating diagnostic log failed: " + diagnosticLogError, true);
            RefreshWindowSources(null, true);
            ReadModelinesFile();
            PopulateModelineDropdown();
            InitializeMiSTerCast();
        }

        void MainWindow_Closing(object sender, CancelEventArgs e)
        {
            if (isStreaming)
                MiSTerCastInterop.StopStream();
            MiSTerCastInterop.Shutdown();
            diagnosticLogWriter?.Dispose();
            diagnosticLogWriter = null;
            helpWindow.Close();
        }

        private void Window_Closed(object sender, EventArgs e)
        {
            Application.Current.Shutdown();
        }

        private void HelpButton_Click(object sender, RoutedEventArgs e)
        {
            helpWindow.Show();
        }

        private void Window_Activated(object sender, EventArgs e)
        {
            if (helpWindow == null)
            {
                helpWindow = new HelpWindow();
                if (!File.Exists(lastSaveFilename))
                {
                    // Show help the first time MiSTerCast is opened
                    helpWindow.Show();
                    try
                    {
                        File.Create(lastSaveFilename);
                    }
                    catch (Exception exception)
                    {
                        Log("Creating last save file failed: " + exception.Message, true);
                    }
                }
                else
                {
                    try
                    {
                        currentSaveFilename = null;
                        using (StreamReader sr = File.OpenText(lastSaveFilename))
                        {
                            if (!sr.EndOfStream)
                            {
                                currentSaveFilename = sr.ReadLine();
                                if (!File.Exists(currentSaveFilename))
                                {
                                    currentSaveFilename = null;
                                    Log("Last save is missing.", true);
                                }
                            }
                        }

                        if (currentSaveFilename != null)
                        {
                            Log("Auto loading settings: " + currentSaveFilename);
                            using (StreamReader sr = File.OpenText(currentSaveFilename))
                            {
                                LoadSaveFileFromStream(sr);
                            }
                        }
                    }
                    catch (Exception exception)
                    {
                        Log("Reading last save file failed: " + exception.Message, true);
                    }
                }
            }
        }

        private async void ToggleStreamButton_Click(object sender, RoutedEventArgs e)
        {
            if (isStreaming)
            {
                if (MiSTerCastInterop.StopStream())
                {
                    isStreaming = false;
                    ToggleStreamButton.Content = "Start Stream";
                    SetCaptureSelectionEnabled(true);
                    SetStreamControlsEnabled(true);
                    EnableAudioCheckBox.IsEnabled = true;
                    ApplyModelineButton.IsEnabled = false;
                }
            }
            else
            {
                if (!isInitialized)
                    InitializeMiSTerCast();

                if (isInitialized)
                {
                    // The modeline gate is the only thing between a mistyped mode
                    // and the FPGA, so refuse to start rather than blit past the
                    // Groovy client's buffer.
                    if (!ValidateCurrentModeline())
                    {
                        Log("Cannot start: the current modeline was rejected.", true);
                        return;
                    }

                    EnablePreviewCheckBox.IsChecked = false;
                    IPAddress ipAddress = null;
                    string target = TargetIpAddresTextBox.Text.Trim();
                    if (!IPAddress.TryParse(target, out ipAddress))
                    {
                        if (target.Length == 0 || target.All(character => Char.IsDigit(character) || character == '.'))
                        {
                            Log("Invalid IPv4 address: " + target, true);
                            return;
                        }

                        ToggleStreamButton.IsEnabled = false;
                        ToggleStreamButton.Content = "Resolving...";
                        try
                        {
                            Task<IPAddress[]> resolveTask = Dns.GetHostAddressesAsync(target);
                            if (await Task.WhenAny(resolveTask, Task.Delay(TimeSpan.FromSeconds(5))) != resolveTask)
                            {
                                Log("Resolving target host timed out after five seconds: " + target, true);
                                return;
                            }

                            ipAddress = (await resolveTask).FirstOrDefault(
                                address => address.AddressFamily == AddressFamily.InterNetwork);
                            if (ipAddress == null)
                            {
                                Log("Target host has no IPv4 address: " + target, true);
                                return;
                            }
                            Log("Resolved target " + target + " to " + ipAddress + ".");
                        }
                        catch (Exception exception)
                        {
                            Log("Resolving target IP address failed: " + exception.Message, true);
                            return;
                        }
                        finally
                        {
                            ToggleStreamButton.IsEnabled = true;
                            ToggleStreamButton.Content = "Start Stream";
                        }
                    }
                    else if (ipAddress.AddressFamily != AddressFamily.InterNetwork)
                    {
                        Log("Only IPv4 target addresses are supported.", true);
                        return;
                    }

                    if (MiSTerCastInterop.StartStream(ipAddress.ToString()))
                    {
                        isStreaming = true;
                        ToggleStreamButton.Content = "Stop Stream";
                        SetCaptureSelectionEnabled(false);
                        EnableAudioCheckBox.IsEnabled = false;
                        // Codec, RGB mode and MTU ride CMD_INIT; they cannot be
                        // changed until the session is torn down and rebuilt.
                        SetStreamControlsEnabled(false);
                    }
                }
            }
        }

        private void SetCaptureSelectionEnabled(bool enabled)
        {
            CaptureModeBox.IsEnabled = enabled;
            CaptureSourceBox.IsEnabled = enabled;
            WindowSourceBox.IsEnabled = enabled;
            RefreshWindowsButton.IsEnabled = enabled;
        }

        #region Stream Options

        // Everything in this region rides CMD_INIT and so takes effect at the
        // next Start Stream, not on the running session.

        private void OnStreamOptionsChanged()
        {
            if (!isInitialized)
                return;

            bool isNlc = CodecComboBox.SelectedIndex == (int)MiSTerCastInterop.Codec.NLC;

            MiSTerCastInterop.SetStreamOptions(
                (byte)CodecComboBox.SelectedIndex,
                (byte)(NlcPackComboBox.SelectedIndex == 1
                    ? MiSTerCastInterop.NlcPack.Rice
                    : MiSTerCastInterop.NlcPack.Tiled),
                (byte)NearLevelComboBox.SelectedIndex,
                (byte)RgbModeComboBox.SelectedIndex,
                (UInt16)(MtuComboBox.SelectedIndex == 1 ? 3800 : 1500),
                AutoReconnectCheckBox.IsChecked.Value,
                (byte)LogLevelComboBox.SelectedIndex,
                AllowOversizeCheckBox.IsChecked.Value);

            NlcPackComboBox.IsEnabled = isNlc && !isStreaming;
            NearLevelComboBox.IsEnabled = isNlc && !isStreaming;

            // The byte budget is in bytes, not pixels, so a change of RGB mode
            // can invalidate a modeline that was fine a moment ago.
            OnModelineChanged();
        }

        private void SetStreamControlsEnabled(bool enabled)
        {
            bool isNlc = CodecComboBox.SelectedIndex == (int)MiSTerCastInterop.Codec.NLC;

            CodecComboBox.IsEnabled = enabled;
            RgbModeComboBox.IsEnabled = enabled;
            MtuComboBox.IsEnabled = enabled;
            LogLevelComboBox.IsEnabled = enabled;
            AutoReconnectCheckBox.IsEnabled = enabled;
            AllowOversizeCheckBox.IsEnabled = enabled;
            NlcPackComboBox.IsEnabled = enabled && isNlc;
            NearLevelComboBox.IsEnabled = enabled && isNlc;
        }

        private void StreamOption_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            OnStreamOptionsChanged();
        }

        private void StreamOption_Checked(object sender, RoutedEventArgs e)
        {
            OnStreamOptionsChanged();
        }

        #endregion Stream Options

        #region Settings

        // Version 1 (shared prefix through CaptureYOffset) and this merge's
        // version 5 are the only formats this build reads back reliably.
        // Versions 2-4 were produced by one of the two pre-merge forks with
        // mutually incompatible field layouts past that shared prefix (one
        // inserted a field before the capture block, the other appended a
        // block after it) - there is no way to tell which produced a given
        // file from the version number alone, so LoadSaveFileFromStream stops
        // reading right after the shared prefix for those and leaves the rest
        // at defaults rather than risk misreading a field.
        const int SettingsVersion = 5;

        private void SaveSettingsButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                SaveFileDialog saveFileDialog = new SaveFileDialog();
                saveFileDialog.Filter = "Settings File|*.sav";
                saveFileDialog.Title = "Save MiSTerCast settings";
                if (currentSaveFilename != null)
                {
                    saveFileDialog.InitialDirectory = Path.GetDirectoryName(currentSaveFilename);
                    saveFileDialog.FileName = Path.GetFileName(currentSaveFilename);
                }

                if (saveFileDialog.ShowDialog().Value)
                {
                    if (!String.IsNullOrWhiteSpace(saveFileDialog.FileName))
                    {
                        using (System.IO.FileStream fs = (System.IO.FileStream)saveFileDialog.OpenFile())
                        {
                            using (var sw = new StreamWriter(fs))
                            {
                                sw.WriteLine(SettingsVersion);

                                sw.WriteLine(TargetIpAddresTextBox.Text);

                                sw.WriteLine(ModelinePresetsBox.SelectedIndex);
                                sw.WriteLine(pclockTextBox.Text);
                                sw.WriteLine(hactiveTextBox.Text);
                                sw.WriteLine(hbeginTextBox.Text);
                                sw.WriteLine(hendTextBox.Text);
                                sw.WriteLine(htotalTextBox.Text);
                                sw.WriteLine(vactiveTextBox.Text);
                                sw.WriteLine(vbeginTextBox.Text);
                                sw.WriteLine(vendTextBox.Text);
                                sw.WriteLine(vtotalTextBox.Text);
                                sw.WriteLine(interlacedCheckBox.IsChecked.Value ? 1 : 0);
                                sw.WriteLine(ProgressiveFramebufferCheckBox.IsChecked.Value ? 1 : 0);

                                sw.WriteLine(CaptureModeBox.SelectedIndex);
                                WindowCaptureSource selectedWindow = WindowSourceBox.SelectedItem as WindowCaptureSource;
                                sw.WriteLine(selectedWindow == null
                                    ? String.Empty
                                    : selectedWindow.Key.Replace("\r", " ").Replace("\n", " "));

                                sw.WriteLine(CaptureSourceBox.SelectedIndex);
                                sw.WriteLine(RotateComboBox.SelectedIndex);
                                sw.WriteLine(EnableAudioCheckBox.IsChecked.Value ? 1 : 0);
                                sw.WriteLine(CropComboBox.SelectedIndex);
                                sw.WriteLine(CaptureWidth.Text);
                                sw.WriteLine(CaptureHeight.Text);
                                sw.WriteLine(CaptureXOffset.Text);
                                sw.WriteLine(CaptureYOffset.Text);
                                sw.WriteLine(SamplingComboBox.SelectedIndex);

                                sw.WriteLine(CodecComboBox.SelectedIndex);
                                sw.WriteLine(NlcPackComboBox.SelectedIndex);
                                sw.WriteLine(NearLevelComboBox.SelectedIndex);
                                sw.WriteLine(RgbModeComboBox.SelectedIndex);
                                sw.WriteLine(MtuComboBox.SelectedIndex);
                                sw.WriteLine(AutoReconnectCheckBox.IsChecked.Value ? 1 : 0);
                                sw.WriteLine(LogLevelComboBox.SelectedIndex);
                                sw.WriteLine(AllowOversizeCheckBox.IsChecked.Value ? 1 : 0);

                                Log("Settings saved.");
                            }
                        }

                        UpdateLastSaveFile(saveFileDialog.FileName);
                    }
                }
            }
            catch (Exception exception)
            {
                Log("Save settings failed: " + exception.Message, true);
            }
        }

        private void LoadSettingsButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                OpenFileDialog openFileDialog = new OpenFileDialog();
                openFileDialog.Filter = "Settings File|*.sav";
                openFileDialog.Title = "Load MiSTerCast settings";
                if (currentSaveFilename != null)
                {
                    openFileDialog.InitialDirectory = Path.GetDirectoryName(currentSaveFilename);
                    openFileDialog.FileName = Path.GetFileName(currentSaveFilename);
                }

                if (openFileDialog.ShowDialog().Value)
                {
                    if (!String.IsNullOrWhiteSpace(openFileDialog.FileName))
                    {
                        using (System.IO.FileStream fs = (System.IO.FileStream)openFileDialog.OpenFile())
                        {
                            using (var sr = new StreamReader(fs))
                            {
                                LoadSaveFileFromStream(sr);
                                UpdateLastSaveFile(openFileDialog.FileName);
                            }
                        }
                    }
                }
            }
            catch (Exception exception)
            {
                Log("Load settings failed: " + exception.Message, true);
            }
        }

        private void LoadSaveFileFromStream(StreamReader sr)
        {
            int settingsVersion = int.Parse(sr.ReadLine());
            if (settingsVersion > SettingsVersion)
            {
                Log("Unsupported save file version: " + settingsVersion, true);
                return;
            }

            TargetIpAddresTextBox.Text = sr.ReadLine();

            ModelinePresetsBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), ModelinePresetsBox.Items.Count - 1);
            pclockTextBox.Text = sr.ReadLine();
            hactiveTextBox.Text = sr.ReadLine();
            hbeginTextBox.Text = sr.ReadLine();
            hendTextBox.Text = sr.ReadLine();
            htotalTextBox.Text = sr.ReadLine();
            vactiveTextBox.Text = sr.ReadLine();
            vbeginTextBox.Text = sr.ReadLine();
            vendTextBox.Text = sr.ReadLine();
            vtotalTextBox.Text = sr.ReadLine();
            interlacedCheckBox.IsChecked = sr.ReadLine() == "1" ? true : false;

            if (settingsVersion >= 2 && settingsVersion < SettingsVersion)
            {
                // A pre-merge fjsj or verbst file: the shared v1 prefix above is
                // identical between both forks, but they diverge from here in
                // mutually incompatible ways. Stop reading rather than guess.
                ProgressiveFramebufferCheckBox.IsChecked = false;
                CodecComboBox.SelectedIndex = (int)MiSTerCastInterop.Codec.LZ4;
                RgbModeComboBox.SelectedIndex = (int)MiSTerCastInterop.RgbMode.Rgb888;
                MtuComboBox.SelectedIndex = 0;
                Log("Loaded a pre-merge settings file (version " + settingsVersion +
                    "): only the modeline was restored. Reconfigure capture and stream options and save again.", true);
                OnStreamOptionsChanged();
                return;
            }

            bool savedProgressiveFramebuffer = settingsVersion >= SettingsVersion && sr.ReadLine() == "1";
            ProgressiveFramebufferCheckBox.IsChecked =
                interlacedCheckBox.IsChecked == true && savedProgressiveFramebuffer;

            int captureMode = 0;
            string savedWindowKey = null;
            if (settingsVersion >= SettingsVersion)
            {
                captureMode = int.Parse(sr.ReadLine());
                savedWindowKey = sr.ReadLine();
            }
            isLoadingCaptureSettings = true;
            CaptureModeBox.SelectedIndex = Math.Max(0, Math.Min(captureMode, CaptureModeBox.Items.Count - 1));
            isLoadingCaptureSettings = false;
            if (CaptureModeBox.SelectedIndex == 1)
            {
                RefreshWindowSources(savedWindowKey, false);
                if (WindowSourceBox.SelectedItem == null)
                    Log("The saved capture window is not currently available. Restore it and refresh the window list.", true);
            }

            CaptureSourceBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), CaptureSourceBox.Items.Count - 1);
            RotateComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), RotateComboBox.Items.Count - 1);
            EnableAudioCheckBox.IsChecked = sr.ReadLine() == "1" ? true : false;
            CropComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), CropComboBox.Items.Count - 1);
            CaptureWidth.Text = sr.ReadLine();
            CaptureHeight.Text = sr.ReadLine();
            CaptureXOffset.Text = sr.ReadLine();
            CaptureYOffset.Text = sr.ReadLine();

            // A version 1 file (the common ancestor format both forks fell back
            // to) ends here - everything below is new to this merge's version 5.
            if (settingsVersion >= SettingsVersion)
            {
                SamplingComboBox.SelectedIndex = Math.Max(0, Math.Min(int.Parse(sr.ReadLine()), SamplingComboBox.Items.Count - 1));

                CodecComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), CodecComboBox.Items.Count - 1);
                NlcPackComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), NlcPackComboBox.Items.Count - 1);
                NearLevelComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), NearLevelComboBox.Items.Count - 1);
                RgbModeComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), RgbModeComboBox.Items.Count - 1);
                MtuComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), MtuComboBox.Items.Count - 1);
                AutoReconnectCheckBox.IsChecked = sr.ReadLine() == "1" ? true : false;
                LogLevelComboBox.SelectedIndex = Math.Min(int.Parse(sr.ReadLine()), LogLevelComboBox.Items.Count - 1);
                AllowOversizeCheckBox.IsChecked = sr.ReadLine() == "1" ? true : false;
            }
            else
            {
                // A version 1 file predates codec/RGB mode/sampling selection; it
                // was written by a build that always used LZ4, RGB888 and Point
                // sampling, so keep that rather than silently moving the user
                // onto NLC or a different sampling mode.
                SamplingComboBox.SelectedIndex = (int)MiSTerCastInterop.SamplingMode.Point;
                CodecComboBox.SelectedIndex = (int)MiSTerCastInterop.Codec.LZ4;
                RgbModeComboBox.SelectedIndex = (int)MiSTerCastInterop.RgbMode.Rgb888;
                MtuComboBox.SelectedIndex = 0;
            }

            OnStreamOptionsChanged();

            Log("Settings loaded.");
        }

        private void UpdateLastSaveFile(string fileName)
        {
            currentSaveFilename = fileName;
            try
            {
                using (FileStream fs = File.OpenWrite(lastSaveFilename))
                {
                    using (StreamWriter sw = new StreamWriter(fs))
                    {
                        sw.WriteLine(fileName);
                    }
                }
            }
            catch (Exception exception)
            {
                Log("Save last save file for autoload failed: " + exception.Message, true);
            }
        }

        #endregion Settings

        #region Number Entry Validation

        private void PositiveIntValidation(object sender, TextCompositionEventArgs e)
        {
            int result;
            e.Handled =
                !(int.TryParse(((TextBox)sender).Text + e.Text, out result) &&
                result >= 0);
        }

        private void IntValidation(object sender, TextCompositionEventArgs e)
        {
            int result;
            string fullText = ((TextBox)sender).Text.Insert(((TextBox)sender).CaretIndex, e.Text);
            e.Handled = !int.TryParse(fullText, out result) && fullText != "-";
        }

        private void PositiveDoubleValidation(object sender, TextCompositionEventArgs e)
        {
            double result;
            e.Handled =
                !(double.TryParse((((TextBox)sender).Text + e.Text).Replace(',','.'), NumberStyles.Any, CultureInfo.InvariantCulture, out result) &&
                result >= 0);
        }

        #endregion Number Entry Validation

        #region Logs

        private MiSTerCastInterop.LogDelegate LogDelegate;

        private string InitializeDiagnosticLog()
        {
            try
            {
                string logDirectory = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "MiSTerCast",
                    "Logs");
                Directory.CreateDirectory(logDirectory);
                diagnosticLogPath = Path.Combine(
                    logDirectory,
                    "MiSTerCast-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
                diagnosticLogWriter = new StreamWriter(
                    new FileStream(diagnosticLogPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite),
                    new UTF8Encoding(false));
                diagnosticLogWriter.AutoFlush = true;
                return null;
            }
            catch (Exception exception)
            {
                diagnosticLogWriter = null;
                return exception.Message;
            }
        }

        private void Log(string message, bool error = false)
        {
            string timestampedMessage = String.Format(
                CultureInfo.InvariantCulture,
                "[{0:yyyy-MM-dd HH:mm:ss.fff}] [{1}] {2}",
                DateTime.Now,
                error ? "ERROR" : "INFO",
                message);
            this.Dispatcher.InvokeAsync(() =>
            {
                try
                {
                    diagnosticLogWriter?.WriteLine(timestampedMessage);
                }
                catch
                {
                    diagnosticLogWriter?.Dispose();
                    diagnosticLogWriter = null;
                }

                bool telemetry = message.StartsWith("[stream]", StringComparison.Ordinal);
                TextBlock logText;
                if (telemetry && telemetryLogText != null)
                {
                    logText = telemetryLogText;
                    logText.Text = message;
                }
                else
                {
                    logText = new TextBlock() { Text = message };
                    LogPanel.Children.Add(logText);
                    if (telemetry)
                        telemetryLogText = logText;
                }
                if (error)
                    logText.Background = Brushes.Pink;
            });
        }

        private void LogCallback(string message, bool error)
        {
            Log(message, error);
        }

        private bool autoScrollLogs = true;
        private void LogScrollView_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            if (e.ExtentHeightChange == 0)
            {
                if (LogScrollView.VerticalOffset == LogScrollView.ScrollableHeight)
                    autoScrollLogs = true;
                else
                    autoScrollLogs = false;
            }

            if (autoScrollLogs && e.ExtentHeightChange != 0)
                LogScrollView.ScrollToVerticalOffset(LogScrollView.ExtentHeight);
        }

        #endregion Logs

        #region Capture Source

        private SourceOptions currentSourceOptions;

        private List<WindowCaptureSource> EnumerateCaptureWindows()
        {
            List<WindowCaptureSource> windows = new List<WindowCaptureSource>();
            uint ownProcessId = (uint)Process.GetCurrentProcess().Id;
            EnumWindows((windowHandle, parameter) =>
            {
                if (!IsWindowVisible(windowHandle))
                    return true;

                int titleLength = GetWindowTextLength(windowHandle);
                if (titleLength <= 0)
                    return true;

                uint processId;
                GetWindowThreadProcessId(windowHandle, out processId);
                if (processId == 0 || processId == ownProcessId)
                    return true;

                int extendedStyle = GetWindowLong(windowHandle, ExtendedWindowStyleIndex);
                bool explicitAppWindow = (extendedStyle & AppWindowStyle) != 0;
                if ((extendedStyle & ToolWindowStyle) != 0 && !explicitAppWindow)
                    return true;
                if (GetWindow(windowHandle, GetOwnerWindow) != IntPtr.Zero && !explicitAppWindow)
                    return true;

                int cloaked;
                if (DwmGetWindowAttribute(
                    windowHandle,
                    DwmWindowAttributeCloaked,
                    out cloaked,
                    sizeof(int)) == 0 && cloaked != 0)
                {
                    return true;
                }

                StringBuilder titleBuilder = new StringBuilder(titleLength + 1);
                if (GetWindowText(windowHandle, titleBuilder, titleBuilder.Capacity) <= 0)
                    return true;

                string title = titleBuilder.ToString().Trim();
                if (title.Length == 0)
                    return true;

                string processName = String.Empty;
                try
                {
                    processName = Process.GetProcessById((int)processId).ProcessName;
                }
                catch
                {
                }

                windows.Add(new WindowCaptureSource
                {
                    Handle = windowHandle,
                    Title = title,
                    ProcessName = processName
                });
                return true;
            }, IntPtr.Zero);

            return windows
                .OrderBy(window => window.Title, StringComparer.CurrentCultureIgnoreCase)
                .ThenBy(window => window.ProcessName, StringComparer.CurrentCultureIgnoreCase)
                .ToList();
        }

        private void RefreshWindowSources(string preferredKey, bool selectFirst)
        {
            WindowCaptureSource previousSelection = WindowSourceBox.SelectedItem as WindowCaptureSource;
            IntPtr previousHandle = previousSelection == null ? IntPtr.Zero : previousSelection.Handle;
            string selectionKey = preferredKey ?? (previousSelection == null ? null : previousSelection.Key);
            List<WindowCaptureSource> windows = EnumerateCaptureWindows();

            WindowCaptureSource selected = windows.FirstOrDefault(window => window.Handle == previousHandle);
            if (selected == null && !String.IsNullOrEmpty(selectionKey))
                selected = windows.FirstOrDefault(window => window.Key == selectionKey);
            if (selected == null && selectFirst)
                selected = windows.FirstOrDefault();

            isRefreshingWindowSources = true;
            WindowSourceBox.ItemsSource = windows;
            WindowSourceBox.SelectedItem = selected;
            isRefreshingWindowSources = false;

            if (CaptureModeBox.SelectedIndex == 1 && isInitialized)
                OnCaptureSourceChanged();
        }

        private void CaptureMode_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (WindowSourcePanel == null || CaptureSourceBox == null)
                return;

            bool captureWindow = CaptureModeBox.SelectedIndex == 1;
            CaptureSourceBox.Visibility = captureWindow ? Visibility.Collapsed : Visibility.Visible;
            WindowSourcePanel.Visibility = captureWindow ? Visibility.Visible : Visibility.Collapsed;
            if (captureWindow && WindowSourceBox.Items.Count == 0)
                RefreshWindowSources(null, true);
            if (captureWindow && !isLoadingCaptureSettings && CropComboBox != null)
                CropComboBox.SelectedIndex = 8;

            if (isInitialized)
                OnCaptureSourceChanged();
        }

        private void WindowSource_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (!isRefreshingWindowSources && isInitialized && CaptureModeBox.SelectedIndex == 1)
                OnCaptureSourceChanged();
        }

        private void RefreshWindowsButton_Click(object sender, RoutedEventArgs e)
        {
            RefreshWindowSources(null, true);
        }

        private void OnCaptureSourceChanged()
        {
            currentSourceOptions.display = (byte)CaptureSourceBox.SelectedIndex;
            currentSourceOptions.alignment = (byte)AlignmentBox.SelectedIndex;
            currentSourceOptions.rotation = (byte)RotateComboBox.SelectedIndex;
            currentSourceOptions.sampling = (byte)SamplingComboBox.SelectedIndex;
            currentSourceOptions.cropmode = (byte)CropComboBox.SelectedIndex;
            CaptureWidth.IsEnabled = CropComboBox.SelectedIndex == 0;
            CaptureHeight.IsEnabled = CropComboBox.SelectedIndex == 0;
            currentSourceOptions.audio = EnableAudioCheckBox.IsChecked.Value;
            currentSourceOptions.preview = EnablePreviewCheckBox.IsChecked.Value;
            ushort.TryParse(CaptureWidth.Text, out currentSourceOptions.width);
            ushort.TryParse(CaptureHeight.Text, out currentSourceOptions.height);
            short.TryParse(CaptureXOffset.Text, out currentSourceOptions.xoffset);
            short.TryParse(CaptureYOffset.Text, out currentSourceOptions.yoffset);

            PreviewImage.Visibility = currentSourceOptions.preview ? Visibility.Visible : Visibility.Hidden;
            PreviewDisabledLabel.Visibility = currentSourceOptions.preview ? Visibility.Hidden : Visibility.Visible;

            IntPtr captureWindowHandle = IntPtr.Zero;
            if (CaptureModeBox.SelectedIndex == 1)
            {
                WindowCaptureSource selectedWindow = WindowSourceBox.SelectedItem as WindowCaptureSource;
                if (selectedWindow == null)
                    return;
                captureWindowHandle = selectedWindow.Handle;
            }
            if (!MiSTerCastInterop.SetCaptureWindow(captureWindowHandle))
                return;

            if (currentSourceOptions.width > 0 && currentSourceOptions.height > 0)
            {
                MiSTerCastInterop.SetSourceEx(
                    currentSourceOptions.display,
                    currentSourceOptions.audio,
                    currentSourceOptions.preview,
                    currentSourceOptions.alignment,
                    currentSourceOptions.cropmode,
                    currentSourceOptions.width,
                    currentSourceOptions.height,
                    currentSourceOptions.xoffset,
                    currentSourceOptions.yoffset,
                    currentSourceOptions.rotation,
                    currentSourceOptions.sampling);
            }
        }

        private void CaptureSource_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (isInitialized)
                OnCaptureSourceChanged();
        }

        private void CaptureSource_Checked(object sender, RoutedEventArgs e)
        {
            if (isInitialized)
                OnCaptureSourceChanged();
        }

        private void CaptureSource_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (isInitialized)
            {
                OnCaptureSourceChanged();
            }
        }

        private void UpdateCropSize()
        {
            CaptureWidth.TextChanged -= CaptureSource_TextChanged;
            CaptureHeight.TextChanged -= CaptureSource_TextChanged;
            switch (currentSourceOptions.cropmode)
            {
                case 1:
                    CaptureWidth.Text = (currentModeLine.hactive).ToString();
                    CaptureHeight.Text = (currentModeLine.vactive).ToString();
                    break;
                case 2:
                    CaptureWidth.Text = (currentModeLine.hactive * 2).ToString();
                    CaptureHeight.Text = (currentModeLine.vactive * 2).ToString();
                    break;
                case 3:
                    CaptureWidth.Text = (currentModeLine.hactive * 3).ToString();
                    CaptureHeight.Text = (currentModeLine.vactive * 3).ToString();
                    break;
                case 4:
                    CaptureWidth.Text = (currentModeLine.hactive * 4).ToString();
                    CaptureHeight.Text = (currentModeLine.vactive * 4).ToString();
                    break;
                case 5:
                    CaptureWidth.Text = (currentModeLine.hactive * 5).ToString();
                    CaptureHeight.Text = (currentModeLine.vactive * 5).ToString();
                    break;
                default:
                    break;
            }
            CaptureWidth.TextChanged += CaptureSource_TextChanged;
            CaptureHeight.TextChanged += CaptureSource_TextChanged;
        }

        #endregion Capture Source

        #region Modelines

        private Modeline currentModeLine;
        private List<Modeline> modelines;

        private void OnModelineChanged()
        {
            double.TryParse(pclockTextBox.Text.Replace(',', '.'), NumberStyles.Any, CultureInfo.InvariantCulture, out currentModeLine.pclock);
            ushort.TryParse(hactiveTextBox.Text, out currentModeLine.hactive);
            ushort.TryParse(hbeginTextBox.Text, out currentModeLine.hbegin);
            ushort.TryParse(hendTextBox.Text, out currentModeLine.hend);
            ushort.TryParse(htotalTextBox.Text, out currentModeLine.htotal);
            ushort.TryParse(vactiveTextBox.Text,  out currentModeLine.vactive);
            ushort.TryParse(vbeginTextBox.Text, out currentModeLine.vbegin);
            ushort.TryParse(vendTextBox.Text, out currentModeLine.vend);
            ushort.TryParse(vtotalTextBox.Text, out currentModeLine.vtotal);
            currentModeLine.interlace = interlacedCheckBox.IsChecked.Value;

            if (isInitialized && ValidateCurrentModeline())
            {
                if (!MiSTerCastInterop.SetModelineEx(
                    currentModeLine.pclock,
                    currentModeLine.hactive,
                    currentModeLine.hbegin,
                    currentModeLine.hend,
                    currentModeLine.htotal,
                    currentModeLine.vactive,
                    currentModeLine.vbegin,
                    currentModeLine.vend,
                    currentModeLine.vtotal,
                    currentModeLine.interlace,
                    currentModeLine.interlace && ProgressiveFramebufferCheckBox.IsChecked == true))
                {
                    return;
                }

                UpdateCropSize();
                OnCaptureSourceChanged();
            }
        }

        // Groovy integration handoff section 4.7. MiSTerCast takes modelines from
        // the user and from modelines.dat with no switchres preset bounding them,
        // so this check is the only thing keeping a bad one off the wire - and a
        // large one inside the client's fixed frame buffer. The native side is the
        // authority (ValidateModelineFor in renderer_nogpu.h); this just surfaces
        // its verdict in the UI.
        private bool ValidateCurrentModeline()
        {
            if (!isInitialized)
                return false;

            byte rgbMode = (byte)RgbModeComboBox.SelectedIndex;
            bool allowOversize = AllowOversizeCheckBox.IsChecked.Value;

            var result = (MiSTerCastInterop.ModelineValidation)MiSTerCastInterop.ValidateModeline(
                currentModeLine.pclock,
                currentModeLine.hactive,
                currentModeLine.hbegin,
                currentModeLine.hend,
                currentModeLine.htotal,
                currentModeLine.vactive,
                currentModeLine.vbegin,
                currentModeLine.vend,
                currentModeLine.vtotal,
                currentModeLine.interlace,
                rgbMode,
                allowOversize);

            string message = null;
            switch (result)
            {
                case MiSTerCastInterop.ModelineValidation.Malformed:
                    message = "Modeline is malformed. The pixel clock must be positive and the blanking must "
                            + "enclose the active area (hbegin >= hactive, hend >= hbegin, htotal > hend, "
                            + "and the same vertically).";
                    break;

                case MiSTerCastInterop.ModelineValidation.OverByteBudget:
                    {
                        int bpp = rgbMode == (byte)MiSTerCastInterop.RgbMode.Rgba8888 ? 4
                                : rgbMode == (byte)MiSTerCastInterop.RgbMode.Rgb565 ? 2 : 3;
                        int bytes = currentModeLine.hactive * currentModeLine.vactive * bpp;
                        if (currentModeLine.interlace && ProgressiveFramebufferCheckBox.IsChecked != true)
                            bytes /= 2;
                        message = string.Format(
                            "Frame is too large for the Groovy client: {0} x {1} x {2} bytes = {3:N0}. "
                            + "Reduce the resolution, use RGB565, or use a (non full-height) interlaced mode.",
                            currentModeLine.hactive, currentModeLine.vactive, bpp, bytes);
                    }
                    break;

                case MiSTerCastInterop.ModelineValidation.OverCrtEnvelope:
                    message = "Modeline is larger than 1024 x 576, which is beyond what a fixed-frequency CRT "
                            + "should be asked to sync. Tick 'Allow oversize modes' if your display can take it.";
                    break;
            }

            if (message == null)
            {
                ModelineWarningText.Visibility = Visibility.Collapsed;
                ModelineWarningText.Text = string.Empty;
            }
            else
            {
                ModelineWarningText.Text = message;
                ModelineWarningText.Visibility = Visibility.Visible;
            }

            // Keep Stop reachable if a mode is edited to something invalid mid-stream.
            ToggleStreamButton.IsEnabled = (message == null) || isStreaming;

            return message == null;
        }

        private void ApplyModelineButton_Click(object sender, RoutedEventArgs e)
        {
            OnModelineChanged();
            ApplyModelineButton.IsEnabled = false;
        }

        private void SetModelineUI(Modeline modeline)
        {
            ignoreModelineChange = true;
            pclockTextBox.Text = modeline.pclock.ToString();
            hactiveTextBox.Text = modeline.hactive.ToString();
            hbeginTextBox.Text = modeline.hbegin.ToString();
            hendTextBox.Text = modeline.hend.ToString();
            htotalTextBox.Text = modeline.htotal.ToString();
            vactiveTextBox.Text = modeline.vactive.ToString();
            vbeginTextBox.Text = modeline.vbegin.ToString();
            vendTextBox.Text = modeline.vend.ToString();
            vtotalTextBox.Text = modeline.vtotal.ToString();
            interlacedCheckBox.IsChecked = modeline.interlace;
            ignoreModelineChange = false;
        }

        private void ModelinePresetsBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {

            if (ModelinePresetsBox.SelectedIndex > 0 && modelines != null && ModelinePresetsBox.SelectedIndex <= modelines.Count)
            {
                SetModelineUI(modelines[ModelinePresetsBox.SelectedIndex - 1]);
                OnModelineChanged();
            }
        }

        void ReadModelinesFile()
        {
            List<Modeline> newModeLines = new List<Modeline>();
            try
            {
                List<string> lines = new List<string>(File.ReadAllLines("modelines.dat"));

                for (int i = 0; i < lines.Count; i++)
                {
                    Modeline modeline = new Modeline();
                    bool badLine = false;
                    string line = lines[i].Trim();
                    if (line.Length == 0 || line[0] == ';')
                    {
                        badLine = true;
                    }
                    else
                    {
                        int nameStart = line.IndexOf('[');
                        int nameEnd = line.IndexOf(']');
                        if (nameStart == -1 || nameEnd == -1 || nameEnd <= nameStart + 1)
                        {
                            Log("Invalid modeline name format: " + lines[i], true);
                            badLine = true;
                        }
                        else
                        {
                            modeline.name = line.Substring(nameStart + 1, nameEnd - nameStart - 1);
                            if (String.IsNullOrEmpty(modeline.name))
                            {
                                Log("Invalid modeline name format: " + lines[i], true);
                                badLine = true;
                            }
                            else
                            {
                                string[] values = line.Remove(nameStart, nameEnd - nameStart + 1)
                                    .Split()
                                    .Select(p => p.Trim())
                                    .Where(p => !string.IsNullOrWhiteSpace(p))
                                    .ToArray();
                                if (values.Length != 10)
                                {
                                    Log("Invalid modeline values count: " + lines[i], true);
                                    badLine = true;
                                }
                                else
                                {
                                    UInt16 interlace;
                                    if (!Double.TryParse(values[0].Replace(',', '.'), NumberStyles.Any, CultureInfo.InvariantCulture,out modeline.pclock) ||
                                        !UInt16.TryParse(values[1], out modeline.hactive) ||
                                        !UInt16.TryParse(values[2], out modeline.hbegin) ||
                                        !UInt16.TryParse(values[3], out modeline.hend) ||
                                        !UInt16.TryParse(values[4], out modeline.htotal) ||
                                        !UInt16.TryParse(values[5], out modeline.vactive) ||
                                        !UInt16.TryParse(values[6], out modeline.vbegin) ||
                                        !UInt16.TryParse(values[7], out modeline.vend) ||
                                        !UInt16.TryParse(values[8], out modeline.vtotal) ||
                                        !UInt16.TryParse(values[9], out interlace))

                                    {
                                        Log("Invalid modeline values format: " + lines[i], true);
                                        badLine = true;
                                    }
                                    else
                                    {
                                        modeline.interlace = interlace != 0;
                                        newModeLines.Add(modeline);
                                    }
                                }
                            }
                        }
                    }

                    if (badLine)
                    {
                        lines.RemoveAt(i);
                        i--;
                    }
                }

                if (newModeLines.Count == 0)
                    throw new Exception("No valid modelines.");

                modelines = newModeLines;
            }
            catch (Exception e)
            {
                Log("Failed to read modelines.dat. " + e.Message, true);
            }
        }

        void PopulateModelineDropdown()
        {
            ModelinePresetsBox.Items.Clear();
            ModelinePresetsBox.Items.Add("Custom");
            foreach (Modeline modeline in modelines)
            {
                ModelinePresetsBox.Items.Add(modeline.name);
            }

            ModelinePresetsBox.SelectedIndex = modelines.Count > 0 ? 1 : 0;
        }

        bool ignoreModelineChange = false;
        private void ModeLineTextBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            OnManualModelineChange();
        }

        private void InterlacedCheckBox_Checked(object sender, RoutedEventArgs e)
        {
            bool interlaced = interlacedCheckBox.IsChecked == true;
            ProgressiveFramebufferCheckBox.IsEnabled = interlaced;
            if (!interlaced && ProgressiveFramebufferCheckBox.IsChecked == true)
            {
                ignoreModelineChange = true;
                ProgressiveFramebufferCheckBox.IsChecked = false;
                ignoreModelineChange = false;
            }
            OnManualModelineChange();
        }

        private void ProgressiveFramebufferCheckBox_Checked(object sender, RoutedEventArgs e)
        {
            OnManualModelineChange();
        }

        private void OnManualModelineChange()
        {
            if (!ignoreModelineChange)
            {
                ModelinePresetsBox.SelectedIndex = 0;
                if (isStreaming)
                    ApplyModelineButton.IsEnabled = true;
            }
        }

        #endregion Modelines

        #region Preview

        private MiSTerCastInterop.CaptureImageDelegate CaptureImageDelegate;
        private bool isPreviewEnabled = true;

        public void CaptureImage(int width, int height, IntPtr buffer)
        {
            if (isPreviewEnabled)
            {
                BitmapSource source = CreateBitmapSource(width, height, buffer);
                source.Freeze();
                this.Dispatcher.InvokeAsync(() =>
                {
                    PreviewImage.Source = source;
                });
            }
        }

        [DllImport("kernel32.dll", EntryPoint = "CopyMemory", SetLastError = false)]
        public static extern void CopyMemory(IntPtr dest, IntPtr src, uint count);

        public BitmapSource CreateBitmapSource(int width, int height, IntPtr buffer)
        {
            WriteableBitmap writableImg = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgra32, null);

            writableImg.Lock();
            CopyMemory(writableImg.BackBuffer, buffer, (uint)(4 * width * height));
            writableImg.AddDirtyRect(new Int32Rect(0, 0, width, height));
            writableImg.Unlock();

            return writableImg;
        }

        #endregion Preview
    }
}

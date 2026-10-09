// Keyboard & mouse bindings editor for the bbport kbm layer (out\SDL3.dll).
// Reads and writes input_config\default.ini and global.ini in shadPS4's input_config format.
// Build: tools\kbm\build_controls.bat (C# compiler of .NET Framework 4, part of Windows).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

static class Program
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        ControlsForm form;
        do { form = new ControlsForm(); Application.Run(form); } while (form.SwitchLanguage);
    }
}

// Interface language: Russian on Russian Windows, English otherwise. The button in the window
// switches it and remembers the choice in input_config\editor-language.txt.
static class L
{
    public static bool En;
    public static string LangFile;
    static bool chosen;

    public static void Init(string langFile)
    {
        LangFile = langFile;
        if (chosen) return;
        chosen = true;
        En = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "ru";
        try
        {
            if (File.Exists(LangFile)) En = File.ReadAllText(LangFile).Trim() == "en";
        }
        catch { }
    }

    public static string T(string ru, string en) { return En ? en : ru; }
}

class Action
{
    public string Output, LabelRu, LabelEn, GroupRu, GroupEn;
    public string Label { get { return L.T(LabelRu, LabelEn); } }
    public string Group { get { return L.T(GroupRu, GroupEn); } }
    public Action(string groupRu, string groupEn, string output, string labelRu, string labelEn)
    {
        GroupRu = groupRu; GroupEn = groupEn; Output = output; LabelRu = labelRu; LabelEn = labelEn;
    }
}

class ControlsForm : Form
{
    static readonly Action[] Actions = {
        new Action("Кнопки", "Buttons", "r1", "R1 — лёгкая атака", "R1 — light attack"),
        new Action("Кнопки", "Buttons", "r2", "R2 — сильная / заряженная атака", "R2 — heavy / charged attack"),
        new Action("Кнопки", "Buttons", "l1", "L1 — трансформация оружия", "L1 — transform weapon"),
        new Action("Кнопки", "Buttons", "l2", "L2 — огнестрел / левая рука", "L2 — firearm / left hand"),
        new Action("Кнопки", "Buttons", "circle", "○ — уклонение / бег (удерживать), на бегу — прыжок", "○ — dodge / sprint (hold), jump while sprinting"),
        new Action("Кнопки", "Buttons", "cross", "✕ — взаимодействие / подтвердить", "✕ — interact / confirm"),
        new Action("Кнопки", "Buttons", "triangle", "△ — флакон крови", "△ — blood vial"),
        new Action("Кнопки", "Buttons", "square", "□ — использовать предмет", "□ — use item"),
        new Action("Кнопки", "Buttons", "l3", "L3 — нажатие левого стика", "L3 — left stick click"),
        new Action("Кнопки", "Buttons", "r3", "R3 — захват цели", "R3 — lock on"),
        new Action("Кнопки", "Buttons", "options", "Options — меню", "Options — menu"),
        new Action("Кнопки", "Buttons", "touchpad_center", "Тачпад (центр) — жесты", "Touchpad (centre) — gestures"),
        new Action("Кнопки", "Buttons", "touchpad_left", "Тачпад слева — жесты (и debug-меню)", "Touchpad left — gestures (and debug menu)"),
        new Action("Кнопки", "Buttons", "touchpad_right", "Тачпад справа — личные вещи", "Touchpad right — personal effects"),
        new Action("Крестовина", "D-pad", "pad_up", "Вверх", "Up"),
        new Action("Крестовина", "D-pad", "pad_down", "Вниз", "Down"),
        new Action("Крестовина", "D-pad", "pad_left", "Влево", "Left"),
        new Action("Крестовина", "D-pad", "pad_right", "Вправо", "Right"),
        new Action("Движение (левый стик)", "Movement (left stick)", "axis_left_y_minus", "Вперёд", "Forward"),
        new Action("Движение (левый стик)", "Movement (left stick)", "axis_left_y_plus", "Назад", "Back"),
        new Action("Движение (левый стик)", "Movement (left stick)", "axis_left_x_minus", "Влево", "Left"),
        new Action("Движение (левый стик)", "Movement (left stick)", "axis_left_x_plus", "Вправо", "Right"),
        new Action("Движение (левый стик)", "Movement (left stick)", "leftjoystick_halfmode", "Шаг (половина хода, удерживать)", "Walk (half tilt, hold)"),
        new Action("Камера (правый стик)", "Camera (right stick)", "axis_right_y_minus", "Вверх", "Up"),
        new Action("Камера (правый стик)", "Camera (right stick)", "axis_right_y_plus", "Вниз", "Down"),
        new Action("Камера (правый стик)", "Camera (right stick)", "axis_right_x_minus", "Влево", "Left"),
        new Action("Камера (правый стик)", "Camera (right stick)", "axis_right_x_plus", "Вправо", "Right"),
        new Action("Камера (правый стик)", "Camera (right stick)", "rightjoystick_halfmode", "Медленная камера (удерживать)", "Slow camera (hold)"),
    };

    const string DefaultConfig =
        "cross = e\ncircle = space\ntriangle = lshift,e\nsquare = r\n" +
        "pad_up = 1\npad_down = 2\npad_left = 3\npad_right = 4\n" +
        "l1 = rightbutton\nl2 = lshift,rightbutton\nr1 = leftbutton\nr2 = lshift,leftbutton\n" +
        "l3 = q\nr3 = middlebutton\n" +
        "options = escape\ntouchpad_center = g\ntouchpad_left = tab\ntouchpad_right = backspace\n" +
        "axis_left_y_minus = w\naxis_left_y_plus = s\naxis_left_x_minus = a\naxis_left_x_plus = d\n" +
        "axis_right_y_minus = up\naxis_right_y_plus = down\naxis_right_x_minus = left\naxis_right_x_plus = right\n" +
        "leftjoystick_halfmode = lctrl\n" +
        "mouse_to_joystick = right\nmouse_movement_params = 0.5, 1.0, 0.125\n";

    static readonly HashSet<string> KeyNames = new HashSet<string>(KeyCapture.AllNames());

    readonly string root, configDir, defaultIni, globalIni;
    readonly Dictionary<string, TextBox[]> boxes = new Dictionary<string, TextBox[]>();
    readonly ComboBox mouseStick = new ComboBox(), stickShape = new ComboBox();
    readonly NumericUpDown smoothing = Num(0, 1000, 10, 0);
    readonly NumericUpDown deadzone = Num(0, 1, 0.01m, 2), speed = Num(0, 20, 0.1m, 2),
                           speedOffset = Num(0, 1, 0.01m, 3), pollMs = Num(1, 200, 1, 0);
    readonly TextBox hotCapture = Box(), hotReload = Box();
    readonly Label status = new Label();
    List<string> otherLines = new List<string>();
    public bool SwitchLanguage;

    static NumericUpDown Num(decimal min, decimal max, decimal inc, int dec)
    {
        return new NumericUpDown { Minimum = min, Maximum = max, Increment = inc, DecimalPlaces = dec, Width = 80 };
    }

    static TextBox Box()
    {
        return new TextBox { ReadOnly = true, Width = 190, BackColor = SystemColors.Window, Cursor = Cursors.Hand };
    }

    public ControlsForm()
    {
        root = Path.GetDirectoryName(typeof(ControlsForm).Assembly.Location);
        if (!File.Exists(Path.Combine(root, "BloodborneLauncher.exe")) && File.Exists(Path.Combine(root, @"..\..\BloodborneLauncher.exe")))
            root = Path.GetFullPath(Path.Combine(root, @"..\.."));
        configDir = Path.Combine(root, "input_config");
        defaultIni = Path.Combine(configDir, "default.ini");
        globalIni = Path.Combine(configDir, "global.ini");
        L.Init(Path.Combine(configDir, "editor-language.txt"));

        Text = L.T("Bloodborne PC — управление (клавиатура и мышь)", "Bloodborne PC — keyboard & mouse controls");
        Font = new Font("Segoe UI", 9f);
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(720, 760);
        MinimumSize = new Size(640, 500);
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(root, "BloodborneLauncher.exe")); } catch { }

        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(12, 8, 12, 8) };
        var table = new TableLayoutPanel { ColumnCount = 3, AutoSize = true, Dock = DockStyle.Top };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 250));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 210));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 210));

        AddHeader(table, L.T("Действие (кнопка геймпада PS4)", "Action (PS4 gamepad button)"), L.T("Клавиша / кнопка мыши", "Key / mouse button"), L.T("Дополнительно", "Alternative"));
        string group = null;
        foreach (var a in Actions)
        {
            if (a.Group != group) { group = a.Group; AddGroup(table, group); }
            var pair = new[] { Box(), Box() };
            boxes[a.Output] = pair;
            table.Controls.Add(new Label { Text = a.Label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 7, 3, 3) });
            for (int i = 0; i < 2; i++) { Hook(pair[i], a.Label); table.Controls.Add(pair[i]); }
        }

        AddGroup(table, L.T("Стики с клавиатуры", "Sticks driven by keys"));
        stickShape.DropDownStyle = ComboBoxStyle.DropDownList;
        stickShape.Items.AddRange(new object[] { L.T("Круг (как у геймпада)", "Circle (like a gamepad)"), L.T("Квадрат (как в shadPS4)", "Square (like shadPS4)") });
        stickShape.Width = 190;
        AddRow(table, L.T("Форма диагоналей", "Diagonal shape"), stickShape, L.T("круг: W+A наклоняет стик как настоящий палец", "circle: W+A tilts the stick like a real thumb"));
        AddRow(table, L.T("Плавность поворота, мс на 90°", "Turn smoothing, ms per 90°"), smoothing, L.T("убирает спотыкание при W→W+A на бегу; 0 — мгновенно", "removes the stumble on W→W+A while sprinting; 0 = instant"));

        AddGroup(table, L.T("Мышь", "Mouse"));
        mouseStick.DropDownStyle = ComboBoxStyle.DropDownList;
        mouseStick.Items.AddRange(new object[] { L.T("Не управляет стиком", "Not used"), L.T("Левый стик", "Left stick"), L.T("Правый стик (камера)", "Right stick (camera)") });
        mouseStick.Width = 190;
        AddRow(table, L.T("Движение мыши управляет", "Mouse movement controls"), mouseStick, null);
        AddRow(table, L.T("Мёртвая зона (deadzone offset, 0–1)", "Deadzone offset (0–1)"), deadzone, L.T("минимальный наклон стика при движении мыши", "minimum stick tilt while the mouse moves"));
        AddRow(table, L.T("Скорость (speed)", "Speed"), speed, L.T("чувствительность камеры", "camera sensitivity"));
        AddRow(table, L.T("Смещение скорости (speed offset, 0–1)", "Speed offset (0–1)"), speedOffset, L.T("добавка к наклону при любом движении", "added to the tilt on any movement"));
        AddRow(table, L.T("Период опроса мыши, мс", "Mouse polling period, ms"), pollMs, L.T("33 — как в shadPS4", "33 = same as shadPS4"));

        AddGroup(table, L.T("Горячие клавиши в игре", "In-game hotkeys"));
        Hook(hotCapture, L.T("Захват мыши вкл/выкл", "Toggle mouse capture"));
        Hook(hotReload, L.T("Перечитать настройки", "Reload settings"));
        AddRow(table, L.T("Захват мыши вкл/выкл", "Toggle mouse capture"), hotCapture, null);
        AddRow(table, L.T("Перечитать настройки без перезапуска", "Reload settings without restarting"), hotReload, null);
        table.Controls.Add(new Label
        {
            Text = L.T("Клик по полю — назначить (Esc — отмена, Delete — очистить). Для комбинации удерживайте Shift/Ctrl/Alt " +
                       "и нажмите клавишу или кнопку мыши. Меню порта: Insert. В меню и вне фокуса мышь освобождается.",
                       "Click a field to bind it (Esc cancels, Delete clears). For a combination hold Shift/Ctrl/Alt " +
                       "and press a key or mouse button. Port menu: Insert. The mouse is released in the menu and when the game loses focus."),
            AutoSize = true, MaximumSize = new Size(660, 0), ForeColor = SystemColors.GrayText, Margin = new Padding(3, 12, 3, 3)
        });
        table.SetColumnSpan(table.Controls[table.Controls.Count - 1], 3);
        scroll.Controls.Add(table);

        var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft, AutoSize = true, Padding = new Padding(8) };
        var save = new Button { Text = L.T("Сохранить", "Save"), AutoSize = true };
        save.Click += (s, e) => Save();
        var import = new Button { Text = L.T("Импорт из shadPS4", "Import from shadPS4"), AutoSize = true };
        import.Click += (s, e) => ImportShad();
        var defaults = new Button { Text = L.T("По умолчанию", "Defaults"), AutoSize = true };
        defaults.Click += (s, e) => { LoadText(DefaultConfig, false); SetStatus(L.T("Загружены настройки по умолчанию (не сохранено)", "Defaults loaded (not saved)")); };
        var reload = new Button { Text = L.T("Отменить изменения", "Revert changes"), AutoSize = true };
        reload.Click += (s, e) => LoadFiles();
        status.AutoSize = true;
        status.Margin = new Padding(3, 8, 20, 3);
        var lang = new Button { Text = L.T("English", "Русский"), AutoSize = true };
        lang.Click += (s, e) =>
        {
            L.En = !L.En;
            try { Directory.CreateDirectory(configDir); File.WriteAllText(L.LangFile, L.En ? "en" : "ru"); } catch { }
            SwitchLanguage = true;
            Close();
        };
        bottom.Controls.AddRange(new Control[] { save, import, defaults, reload, lang, status });

        Controls.Add(scroll);
        Controls.Add(bottom);
        LoadFiles();
    }

    void AddHeader(TableLayoutPanel t, params string[] texts)
    {
        foreach (var s in texts) t.Controls.Add(new Label { Text = s, AutoSize = true, Font = new Font(Font, FontStyle.Bold), Margin = new Padding(3, 3, 3, 6) });
    }

    void AddGroup(TableLayoutPanel t, string text)
    {
        var l = new Label { Text = text, AutoSize = true, Font = new Font(Font.FontFamily, 10.5f, FontStyle.Bold), ForeColor = Color.FromArgb(140, 20, 20), Margin = new Padding(3, 14, 3, 4) };
        t.Controls.Add(l);
        t.SetColumnSpan(l, 3);
    }

    void AddRow(TableLayoutPanel t, string label, Control c, string hint)
    {
        t.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 7, 3, 3) });
        t.Controls.Add(c);
        t.Controls.Add(new Label { Text = hint ?? "", AutoSize = true, Anchor = AnchorStyles.Left, ForeColor = SystemColors.GrayText, MaximumSize = new Size(205, 0) });
    }

    void Hook(TextBox box, string title)
    {
        box.MouseUp += (s, e) =>
        {
            using (var cap = new KeyCapture(title))
            {
                if (cap.ShowDialog(this) == DialogResult.OK) { box.Text = cap.Result; MarkConflicts(); }
            }
        };
        box.KeyDown += (s, e) => { if (e.KeyCode == Keys.Delete || e.KeyCode == Keys.Back) { box.Text = ""; MarkConflicts(); } };
    }

    void SetStatus(string s) { status.Text = s; }

    void MarkConflicts()
    {
        var all = boxes.Values.SelectMany(b => b).Where(b => b.Text != "").ToList();
        foreach (var b in all)
            b.BackColor = all.Count(o => o.Text == b.Text) > 1 ? Color.FromArgb(255, 220, 220) : SystemColors.Window;
        foreach (var b in boxes.Values.SelectMany(b => b).Where(b => b.Text == "")) b.BackColor = SystemColors.Window;
    }

    // ---- reading -------------------------------------------------------------------------------

    static bool ParseLine(string raw, out string output, out string input)
    {
        output = input = null;
        string line = raw;
        int hash = line.IndexOf('#');
        if (hash >= 0) line = line.Substring(0, hash);
        int eq = line.IndexOf('=');
        if (eq < 0) return false;
        output = line.Substring(0, eq).Trim().ToLowerInvariant();
        input = string.Join(",", line.Substring(eq + 1).Split(',').Select(x => x.Trim().ToLowerInvariant()).Where(x => x != ""));
        return output != "" && input != "";
    }

    static bool IsKeyboardInput(string input)
    {
        return input.Split(',').All(k => KeyNames.Contains(k));
    }

    void LoadFiles()
    {
        string text = File.Exists(defaultIni) ? File.ReadAllText(defaultIni) : DefaultConfig;
        LoadText(text, true);
        LoadGlobal(File.Exists(globalIni) ? File.ReadAllText(globalIni) : "");
        SetStatus(File.Exists(defaultIni) ? L.T("Загружено: input_config\\default.ini", "Loaded: input_config\\default.ini") : L.T("Файла настроек нет — показаны настройки по умолчанию", "No settings file yet — showing the defaults"));
    }

    void LoadText(string text, bool keepOther)
    {
        foreach (var pair in boxes.Values) { pair[0].Text = ""; pair[1].Text = ""; }
        mouseStick.SelectedIndex = 0;
        stickShape.SelectedIndex = 0;
        smoothing.Value = 100;
        deadzone.Value = 0.5m; speed.Value = 1m; speedOffset.Value = 0.125m; pollMs.Value = 33;
        var other = new List<string>();
        foreach (var raw in text.Replace("\r", "").Split('\n'))
        {
            string o, i;
            if (!ParseLine(raw, out o, out i)) { continue; }
            if (o == "mouse_to_joystick") { mouseStick.SelectedIndex = i == "left" ? 1 : i == "right" ? 2 : 0; continue; }
            if (o == "mouse_movement_params")
            {
                var p = i.Split(',');
                decimal v;
                if (p.Length >= 3)
                {
                    if (TryDec(p[0], out v)) deadzone.Value = Clamp(deadzone, v);
                    if (TryDec(p[1], out v)) speed.Value = Clamp(speed, v);
                    if (TryDec(p[2], out v)) speedOffset.Value = Clamp(speedOffset, v);
                }
                continue;
            }
            if (o == "stick_shape") { stickShape.SelectedIndex = i == "square" ? 1 : 0; continue; }
            if (o == "stick_smoothing_ms") { decimal v; if (TryDec(i, out v)) smoothing.Value = Clamp(smoothing, v); continue; }
            if (o == "mouse_poll_ms") { decimal v; if (TryDec(i, out v)) pollMs.Value = Clamp(pollMs, v); continue; }
            TextBox[] pair;
            if (boxes.TryGetValue(o, out pair) && IsKeyboardInput(i))
            {
                if (pair[0].Text == "") pair[0].Text = i;
                else if (pair[1].Text == "") pair[1].Text = i;
                continue;
            }
            other.Add(raw.Trim());
        }
        if (keepOther) otherLines = other;
        MarkConflicts();
    }

    void LoadGlobal(string text)
    {
        hotCapture.Text = "f7"; hotReload.Text = "f8";
        foreach (var raw in text.Replace("\r", "").Split('\n'))
        {
            string o, i;
            if (!ParseLine(raw, out o, out i)) continue;
            if (o == "hotkey_toggle_mouse_to_joystick") hotCapture.Text = i;
            if (o == "hotkey_reload_inputs") hotReload.Text = i;
        }
    }

    static bool TryDec(string s, out decimal v)
    {
        return decimal.TryParse(s.Trim(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v);
    }

    static decimal Clamp(NumericUpDown n, decimal v) { return Math.Max(n.Minimum, Math.Min(n.Maximum, v)); }

    void ImportShad()
    {
        string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), @"shadPS4\input_config");
        string game = Path.Combine(dir, "CUSA03173.ini"), def = Path.Combine(dir, "default.ini");
        var dlg = new OpenFileDialog { Title = L.T("Конфиг управления shadPS4", "shadPS4 input config"), Filter = "shadPS4 input config (*.ini)|*.ini", InitialDirectory = Directory.Exists(dir) ? dir : "" };
        if (File.Exists(game)) dlg.FileName = game; else if (File.Exists(def)) dlg.FileName = def;
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        LoadText(File.ReadAllText(dlg.FileName), true);
        string g = Path.Combine(Path.GetDirectoryName(dlg.FileName), "global.ini");
        if (File.Exists(g)) LoadGlobal(File.ReadAllText(g));
        SetStatus(L.T("Импортировано из ", "Imported from ") + Path.GetFileName(dlg.FileName) + L.T(" — нажмите «Сохранить»", " — press Save"));
    }

    // ---- writing -------------------------------------------------------------------------------

    void Save()
    {
        Directory.CreateDirectory(configDir);
        var sb = new StringBuilder();
        sb.AppendLine("# Bloodborne PC keyboard & mouse bindings (shadPS4 input_config format).");
        sb.AppendLine("# Edited by BloodborneControls.exe; F8 in the game reloads this file.");
        sb.AppendLine();
        string group = null;
        foreach (var a in Actions)
        {
            if (a.GroupEn != group) { group = a.GroupEn; sb.AppendLine(); sb.AppendLine("# " + group); }
            foreach (var b in boxes[a.Output]) if (b.Text != "") sb.AppendLine(a.Output + " = " + b.Text);
        }
        sb.AppendLine();
        sb.AppendLine("# Keyboard-driven sticks (bbport): circle or square, ms per 90 degree turn");
        sb.AppendLine("stick_shape = " + (stickShape.SelectedIndex == 1 ? "square" : "circle"));
        sb.AppendLine("stick_smoothing_ms = " + smoothing.Value);
        sb.AppendLine();
        sb.AppendLine("# Mouse");
        sb.AppendLine("mouse_to_joystick = " + new[] { "none", "left", "right" }[mouseStick.SelectedIndex]);
        sb.AppendLine(string.Format(System.Globalization.CultureInfo.InvariantCulture, "mouse_movement_params = {0:0.00}, {1:0.0##}, {2:0.000}", deadzone.Value, speed.Value, speedOffset.Value));
        if (pollMs.Value != 33) sb.AppendLine("mouse_poll_ms = " + pollMs.Value);
        if (otherLines.Count > 0)
        {
            sb.AppendLine();
            sb.AppendLine("# Controller bindings and other settings");
            foreach (var l in otherLines) sb.AppendLine(l);
        }
        File.WriteAllText(defaultIni, sb.ToString(), new UTF8Encoding(false));

        var lines = File.Exists(globalIni) ? File.ReadAllLines(globalIni).ToList() : new List<string> { "# Loaded alongside default.ini." };
        SetHotkey(lines, "hotkey_toggle_mouse_to_joystick", hotCapture.Text);
        SetHotkey(lines, "hotkey_reload_inputs", hotReload.Text);
        File.WriteAllLines(globalIni, lines, new UTF8Encoding(false));
        SetStatus(L.T("Сохранено. Если игра запущена — нажмите в ней ", "Saved. If the game is running, press ") + hotReload.Text.ToUpperInvariant() + L.T("", " in it"));
    }

    static void SetHotkey(List<string> lines, string name, string key)
    {
        int idx = lines.FindIndex(l => { string o, i; return ParseLine(l, out o, out i) && o == name; });
        if (key == "") { if (idx >= 0) lines.RemoveAt(idx); return; }
        if (idx >= 0) lines[idx] = name + " = " + key; else lines.Add(name + " = " + key);
    }
}

// Modal "press a key" window: keyboard scancodes (layout independent), mouse buttons and wheel,
// with Shift/Ctrl/Alt held for combinations.
class KeyCapture : Form, IMessageFilter
{
    static readonly Dictionary<int, string> Scan = new Dictionary<int, string>();
    static readonly string[] Mouse = { "leftbutton", "rightbutton", "middlebutton", "sidebuttonback", "sidebuttonforward",
                                       "mousewheelup", "mousewheeldown", "mousewheelleft", "mousewheelright" };
    static readonly string[] Modifiers = { "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt" };

    static KeyCapture()
    {
        string row1 = "1234567890";
        for (int i = 0; i < 10; i++) Scan[0x02 + i] = row1[i].ToString();
        string[] letters = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
        int[] starts = { 0x10, 0x1E, 0x2C };
        for (int r = 0; r < 3; r++) for (int i = 0; i < letters[r].Length; i++) Scan[starts[r] + i] = letters[r][i].ToString();
        for (int i = 0; i < 10; i++) Scan[0x3B + i] = "f" + (i + 1);
        var map = new Dictionary<int, string> {
            {0x01,"escape"},{0x0C,"minus"},{0x0D,"equals"},{0x0E,"backspace"},{0x0F,"tab"},{0x1A,"lbracket"},{0x1B,"rbracket"},
            {0x1C,"enter"},{0x1D,"lctrl"},{0x27,"semicolon"},{0x28,"apostrophe"},{0x29,"grave"},{0x2A,"lshift"},{0x2B,"backslash"},
            {0x33,"comma"},{0x34,"period"},{0x35,"slash"},{0x36,"rshift"},{0x37,"kpasterisk"},{0x38,"lalt"},{0x39,"space"},
            {0x3A,"capslock"},{0x46,"scrolllock"},{0x47,"kp7"},{0x48,"kp8"},{0x49,"kp9"},{0x4A,"kpminus"},{0x4B,"kp4"},{0x4C,"kp5"},
            {0x4D,"kp6"},{0x4E,"kpplus"},{0x4F,"kp1"},{0x50,"kp2"},{0x51,"kp3"},{0x52,"kp0"},{0x53,"kpperiod"},{0x57,"f11"},{0x58,"f12"},
            // extended (E0) keys: 0x100 | scancode
            {0x11C,"kpenter"},{0x11D,"rctrl"},{0x135,"kpslash"},{0x137,"printscreen"},{0x138,"ralt"},{0x147,"home"},{0x148,"up"},
            {0x149,"pgup"},{0x14B,"left"},{0x14D,"right"},{0x14F,"end"},{0x150,"down"},{0x151,"pgdown"},{0x152,"insert"},
            {0x153,"delete"},{0x15B,"lwin"},{0x15C,"rwin"},
        };
        foreach (var kv in map) Scan[kv.Key] = kv.Value;
    }

    public static IEnumerable<string> AllNames()
    {
        return Scan.Values.Concat(Mouse).Concat(new[] { "pausebreak", "kpequals", "kpcomma", "lmeta", "rmeta", "tilde", "exclamation",
            "at", "hash", "dollar", "percent", "caret", "ampersand", "asterisk", "lparen", "rparen", "underscore", "plus", "lbrace",
            "rbrace", "pipe", "colon", "quote", "less", "greater", "question" });
    }

    public string Result;
    readonly List<string> held = new List<string>();
    readonly Label info;

    public KeyCapture(string title)
    {
        Text = L.T("Назначение: ", "Bind: ") + title;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(420, 150);
        KeyPreview = true;
        info = new Label
        {
            Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter, Font = new Font("Segoe UI", 10.5f),
            Text = L.T("Нажмите клавишу, кнопку мыши или прокрутите колесо в этом окне.\n" +
                       "Комбинация: удерживайте Shift / Ctrl / Alt.\nEsc — отмена",
                       "Press a key or mouse button, or scroll the wheel in this window.\n" +
                       "Combination: hold Shift / Ctrl / Alt.\nEsc cancels")
        };
        Controls.Add(info);
        Application.AddMessageFilter(this);
        FormClosed += (s, e) => Application.RemoveMessageFilter(this);
    }

    void Finish(string key)
    {
        var keys = held.Where(m => m != key).Take(2).ToList();
        keys.Add(key);
        Result = string.Join(",", keys);
        DialogResult = DialogResult.OK;
    }

    public bool PreFilterMessage(ref Message m)
    {
        if (!ContainsFocus && m.HWnd != Handle && !IsChild(m.HWnd)) return false;
        switch (m.Msg)
        {
        case 0x100: case 0x104: // WM_KEYDOWN, WM_SYSKEYDOWN
        {
            long lp = m.LParam.ToInt64();
            int code = (int)((lp >> 16) & 0xFF) | (((lp >> 24) & 1) != 0 ? 0x100 : 0);
            string name;
            if (!Scan.TryGetValue(code, out name)) return true;
            if (((lp >> 30) & 1) != 0) return true; // auto-repeat
            if (name == "escape" && held.Count == 0) { DialogResult = DialogResult.Cancel; return true; }
            if (Modifiers.Contains(name))
            {
                if (!held.Contains(name)) held.Add(name);
                info.Text = string.Join(" + ", held) + " + …\n" + L.T("Отпустите, чтобы назначить только модификатор", "Release to bind the modifier alone");
                return true;
            }
            Finish(name);
            return true;
        }
        case 0x101: case 0x105: // WM_KEYUP, WM_SYSKEYUP: a lone modifier
        {
            long lp = m.LParam.ToInt64();
            int code = (int)((lp >> 16) & 0xFF) | (((lp >> 24) & 1) != 0 ? 0x100 : 0);
            string name;
            if (Scan.TryGetValue(code, out name) && held.Contains(name)) { held.Remove(name); Finish(name); }
            return true;
        }
        case 0x201: Finish("leftbutton"); return true;
        case 0x204: Finish("rightbutton"); return true;
        case 0x207: Finish("middlebutton"); return true;
        case 0x20B: Finish(((m.WParam.ToInt64() >> 16) & 0xFFFF) == 1 ? "sidebuttonback" : "sidebuttonforward"); return true;
        case 0x20A: Finish((short)((m.WParam.ToInt64() >> 16) & 0xFFFF) > 0 ? "mousewheelup" : "mousewheeldown"); return true;
        case 0x20E: Finish((short)((m.WParam.ToInt64() >> 16) & 0xFFFF) > 0 ? "mousewheelright" : "mousewheelleft"); return true;
        }
        return false;
    }

    bool IsChild(IntPtr h)
    {
        foreach (Control c in Controls) if (c.Handle == h) return true;
        return false;
    }
}

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Xml;
using Mesen.Config;
using Mesen.Interop;

namespace Mesen.Localization
{
	class ResourceHelper
	{
		private const string DefaultLanguageCode = "en";
		private const string SimplifiedChineseLanguageCode = "zh-cn";
		private const string JapaneseLanguageCode = "ja";
		private const string ResourcePrefix = "Mesen.Localization.resources.";
		private const string ResourceSuffix = ".xml";

		private static XmlDocument _resources = new XmlDocument();

		private static Dictionary<Enum, string> _enumLabelCache = new();
		private static Dictionary<string, string> _viewLabelCache = new();
		private static Dictionary<string, string> _messageCache = new();

		public static void LoadResources()
		{
			try {
				_enumLabelCache.Clear();
				_viewLabelCache.Clear();
				_messageCache.Clear();

				LoadResourceFile(DefaultLanguageCode, true);
				string language = ConfigManager.Config.Preferences.Language;
				if(!string.Equals(language, DefaultLanguageCode, StringComparison.OrdinalIgnoreCase)) {
					LoadResourceFile(language, false);
				}
			} catch {
			}
		}

		private static void LoadResourceFile(string language, bool updateResourceDocument)
		{
			string? resourceData = GetResourceFileContent(language);
			if(string.IsNullOrWhiteSpace(resourceData)) {
				return;
			}

			XmlDocument resources = new XmlDocument();
			resources.LoadXml(resourceData);
			if(updateResourceDocument) {
				_resources = resources;
			}

			foreach(XmlNode node in resources.SelectNodes("/Resources/Messages/Message")!) {
				_messageCache[node.Attributes!["ID"]!.Value] = node.InnerText;
			}

#pragma warning disable IL2026 // Members annotated with 'RequiresUnreferencedCodeAttribute' require dynamic access otherwise can break functionality when trimming application code
			Dictionary<string, Type> enumTypes = Assembly.GetExecutingAssembly().GetTypes().Where(t => t.IsEnum).ToDictionary(t => t.Name);
#pragma warning restore IL2026 // Members annotated with 'RequiresUnreferencedCodeAttribute' require dynamic access otherwise can break functionality when trimming application code

			foreach(XmlNode node in resources.SelectNodes("/Resources/Enums/Enum")!) {
				string enumName = node.Attributes!["ID"]!.Value;
				if(enumTypes.TryGetValue(enumName, out Type? enumType)) {
					foreach(XmlNode enumNode in node.ChildNodes) {
						if(Enum.TryParse(enumType, enumNode.Attributes!["ID"]!.Value, out object? value)) {
							_enumLabelCache[(Enum)value!] = enumNode.InnerText;
						}
					}
				} else {
					throw new Exception("Unknown enum type: " + enumName);
				}
			}

			foreach(XmlNode node in resources.SelectNodes("/Resources/Forms/Form")!) {
				string viewName = node.Attributes!["ID"]!.Value;
				foreach(XmlNode formNode in node.ChildNodes) {
					if(formNode is XmlElement elem) {
						_viewLabelCache[viewName + "_" + elem.Attributes!["ID"]!.Value] = elem.InnerText;
					}
				}
			}
		}

		private static string? GetResourceFileContent(string language)
		{
			string normalizedLanguage = NormalizeLanguageCode(language);
			Assembly assembly = Assembly.GetExecutingAssembly();
			string resourceName = ResourcePrefix + normalizedLanguage + ResourceSuffix;
			using(Stream? stream = assembly.GetManifestResourceStream(resourceName)) {
				if(stream != null) {
					using StreamReader reader = new StreamReader(stream);
					return reader.ReadToEnd();
				}
			}

			string externalPath = Path.Combine(ConfigManager.HomeFolder, "Localization", "resources." + normalizedLanguage + ".xml");
			if(File.Exists(externalPath)) {
				return File.ReadAllText(externalPath);
			}

			return null;
		}

		private static string NormalizeLanguageCode(string language)
		{
			if(string.IsNullOrWhiteSpace(language)) {
				return DefaultLanguageCode;
			}
			return language.Trim().Replace('_', '-').ToLowerInvariant();
		}

		public static List<LanguageOption> GetAvailableLanguages()
		{
			HashSet<string> languages = new() { DefaultLanguageCode, SimplifiedChineseLanguageCode, JapaneseLanguageCode };
			Assembly assembly = Assembly.GetExecutingAssembly();
			foreach(string resourceName in assembly.GetManifestResourceNames()) {
				if(resourceName.StartsWith(ResourcePrefix) && resourceName.EndsWith(ResourceSuffix)) {
					languages.Add(resourceName.Substring(ResourcePrefix.Length, resourceName.Length - ResourcePrefix.Length - ResourceSuffix.Length));
				}
			}

			string externalFolder = Path.Combine(ConfigManager.HomeFolder, "Localization");
			if(Directory.Exists(externalFolder)) {
				foreach(string file in Directory.EnumerateFiles(externalFolder, "resources.*.xml")) {
					string filename = Path.GetFileName(file);
					languages.Add(filename.Substring("resources.".Length, filename.Length - "resources.".Length - ResourceSuffix.Length));
				}
			}

			return languages
				.Select(language => new LanguageOption(NormalizeLanguageCode(language), GetLanguageDisplayName(language)))
				.OrderBy(language => language.DisplayName)
				.ToList();
		}

		private static string GetLanguageDisplayName(string language)
		{
			string normalizedLanguage = NormalizeLanguageCode(language);
			if(normalizedLanguage == DefaultLanguageCode) {
				return "English";
			} else if(normalizedLanguage == SimplifiedChineseLanguageCode) {
				return "中文（简体）";
			} else if(normalizedLanguage == JapaneseLanguageCode) {
				return "日本語";
			}

			try {
				CultureInfo culture = CultureInfo.GetCultureInfo(normalizedLanguage);
				return culture.NativeName;
			} catch {
				return normalizedLanguage;
			}
		}

		public static string GetMessage(string id, params object[] args)
		{
			if(_messageCache.TryGetValue(id, out string? text)) {
				return string.Format(text, args);
			} else {
				return "[[" + id + "]]";
			}
		}

		public static string GetEnumText(Enum e)
		{
			if(_enumLabelCache.TryGetValue(e, out string? text)) {
				return text;
			} else {
				return "[[" + e.ToString() + "]]";
			}
		}

		public static Enum[] GetEnumValues(Type t)
		{
			List<Enum> values = new List<Enum>();
			XmlNode? node = _resources.SelectSingleNode("/Resources/Enums/Enum[@ID='" + t.Name + "']");
			if(node?.Attributes!["ID"]!.Value == t.Name) {
				foreach(XmlNode enumNode in node.ChildNodes) {
					if(Enum.TryParse(t, enumNode.Attributes!["ID"]!.Value, out object? value) && value != null) {
						values.Add((Enum)value);
					}
				}
			}
			return values.ToArray();
		}

		public static string GetViewLabel(string view, string control)
		{
			if(_viewLabelCache.TryGetValue(view + "_" + control, out string? text)) {
				return text;
			} else {
				return $"[{view}:{control}]";
			}
		}
	}

	public record LanguageOption(string Code, string DisplayName);
}

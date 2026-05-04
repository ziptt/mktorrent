#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace lt = libtorrent;

namespace
{
enum class torrent_mode
{
	hybrid,
	v1_only,
	v2_only
};

struct options
{
	std::string input;
	std::string output;
	bool output_set = false;
	bool write_torrent = false;
	bool print_magnet = false;
	bool quiet = false;
	bool private_torrent = false;
	bool no_attributes = false;
	bool include_mtime = false;
	bool preserve_symlinks = false;
	int piece_size = 0;
	torrent_mode mode = torrent_mode::hybrid;
	std::string comment;
	std::string creator = "mktorrent";
	std::vector<std::string> trackers;
	std::vector<std::string> url_seeds;
	std::vector<std::string> http_seeds;
	std::vector<std::pair<std::string, int>> dht_nodes;
};

struct cli_exit
{
	int code;
};

[[noreturn]] void fail(std::string const& message)
{
	throw std::runtime_error(message);
}

#ifdef _WIN32
std::string wide_to_utf8(std::wstring const& value)
{
	if (value.empty()) return {};

	int const bytes = WideCharToMultiByte(
		CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) fail("failed to convert Windows wide string to UTF-8");

	std::string result(static_cast<std::size_t>(bytes), '\0');
	int const written = WideCharToMultiByte(
		CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
	if (written != bytes) fail("failed to convert Windows wide string to UTF-8");
	return result;
}

std::wstring utf8_to_wide(std::string const& value)
{
	if (value.empty()) return {};

	int const chars = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (chars <= 0) fail("invalid UTF-8 path or argument");

	std::wstring result(static_cast<std::size_t>(chars), L'\0');
	int const written = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), chars);
	if (written != chars) fail("invalid UTF-8 path or argument");
	return result;
}
#endif

fs::path make_path(std::string const& value)
{
#ifdef _WIN32
	return fs::path(utf8_to_wide(value));
#else
	return fs::path(value);
#endif
}

std::string path_to_utf8(fs::path const& path)
{
#ifdef _WIN32
	return wide_to_utf8(path.wstring());
#else
	return path.string();
#endif
}

std::string to_lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

bool starts_with_dash(std::string const& value)
{
	return !value.empty() && value[0] == '-';
}

bool is_power_of_two(std::uint64_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t parse_u64(std::string const& value, std::string const& option_name)
{
	if (value.empty()) fail(option_name + " requires a number");

	std::uint64_t result = 0;
	for (char const c : value)
	{
		if (!std::isdigit(static_cast<unsigned char>(c)))
			fail(option_name + " requires a non-negative integer");

		std::uint64_t const digit = static_cast<std::uint64_t>(c - '0');
		if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
			fail(option_name + " is too large");
		result = result * 10 + digit;
	}
	return result;
}

int parse_piece_size(std::string const& value)
{
	if (value.empty()) fail("--piece-size requires a value");

	std::size_t pos = 0;
	while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) ++pos;
	if (pos == 0) fail("--piece-size must start with a number");

	std::uint64_t const number = parse_u64(value.substr(0, pos), "--piece-size");
	std::string suffix = to_lower(value.substr(pos));

	std::uint64_t multiplier = 1;
	if (suffix.empty() || suffix == "b")
	{
		multiplier = 1;
	}
	else if (suffix == "k" || suffix == "kb" || suffix == "kib")
	{
		multiplier = 1024ULL;
	}
	else if (suffix == "m" || suffix == "mb" || suffix == "mib")
	{
		multiplier = 1024ULL * 1024ULL;
	}
	else if (suffix == "g" || suffix == "gb" || suffix == "gib")
	{
		multiplier = 1024ULL * 1024ULL * 1024ULL;
	}
	else
	{
		fail("unsupported --piece-size suffix: " + suffix);
	}

	if (number > std::numeric_limits<std::uint64_t>::max() / multiplier)
		fail("--piece-size is too large");

	std::uint64_t const bytes = number * multiplier;
	if (!is_power_of_two(bytes)) fail("--piece-size must be a power of two");
	if (bytes < 16ULL * 1024ULL) fail("--piece-size must be at least 16 KiB");
	if (bytes > 128ULL * 1024ULL * 1024ULL) fail("--piece-size must be at most 128 MiB");

	return static_cast<int>(bytes);
}

int parse_port(std::string const& value)
{
	std::uint64_t const port = parse_u64(value, "DHT node port");
	if (port == 0 || port > 65535) fail("DHT node port must be in the range 1..65535");
	return static_cast<int>(port);
}

std::pair<std::string, int> parse_dht_node(std::string const& value)
{
	if (value.empty()) fail("--dht-node requires HOST:PORT");

	if (value.front() == '[')
	{
		std::size_t const close = value.find(']');
		if (close == std::string::npos || close + 2 > value.size() || value[close + 1] != ':')
			fail("--dht-node IPv6 values must use [address]:port syntax");

		std::string host = value.substr(1, close - 1);
		if (host.empty()) fail("--dht-node host is empty");
		return {host, parse_port(value.substr(close + 2))};
	}

	std::size_t const colon = value.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 == value.size())
		fail("--dht-node requires HOST:PORT");

	if (value.find(':') != colon)
		fail("--dht-node IPv6 values must use [address]:port syntax");

	return {value.substr(0, colon), parse_port(value.substr(colon + 1))};
}

std::string format_bytes(std::int64_t bytes)
{
	static char const* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	double value = static_cast<double>(bytes);
	std::size_t unit = 0;
	while (value >= 1024.0 && unit + 1 < std::size(units))
	{
		value /= 1024.0;
		++unit;
	}

	std::string out = std::to_string(value);
	std::size_t dot = out.find('.');
	if (dot != std::string::npos)
	{
		std::size_t keep = unit == 0 ? dot : dot + 2;
		if (keep < out.size()) out.erase(keep);
		while (!out.empty() && out.back() == '0') out.pop_back();
		if (!out.empty() && out.back() == '.') out.pop_back();
	}
	return out + " " + units[unit];
}

void print_usage(std::ostream& os)
{
	os << "Usage: mktorrent --input PATH (--magnet | --output FILE) [options]\n"
	   << "       mktorrent PATH (--magnet | --output FILE) [options]\n\n"
	   << "Create a .torrent file, a magnet link, or both.\n\n"
	   << "Options:\n"
	   << "  -i, --input PATH          File or directory to package\n"
	   << "  -o, --output FILE         Write a .torrent file\n"
	   << "  -m, --magnet              Print the magnet link\n"
	   << "  -t, --tracker URL         Add tracker URL (repeatable)\n"
	   << "      --web-seed URL        Add BEP 19 web seed URL (repeatable)\n"
	   << "      --http-seed URL       Add HTTP seed URL (repeatable)\n"
	   << "      --dht-node HOST:PORT  Add DHT bootstrap node (repeatable)\n"
	   << "      --comment TEXT        Set torrent comment\n"
	   << "      --creator TEXT        Set creator string (default: mktorrent)\n"
	   << "      --private             Set the private flag\n"
	   << "      --piece-size SIZE     Piece size, e.g. 256K or 4M (default: automatic)\n"
	   << "      --v1-only             Create BitTorrent v1 metadata only\n"
	   << "      --v2-only             Create BitTorrent v2 metadata only\n"
	   << "      --hybrid              Create hybrid v1/v2 metadata (default)\n"
	   << "      --no-attributes       Ignore filesystem attributes in metadata\n"
	   << "      --mtime               Include file modification times\n"
	   << "      --symlinks            Store symlinks as symlinks instead of file data\n"
	   << "  -q, --quiet               Suppress progress and summary output\n"
	   << "  -h, --help                Show this help text\n"
	   << "      --version             Show program and libtorrent version\n";
}

std::string take_value(
	std::vector<std::string> const& args,
	std::size_t& index,
	std::string const& current,
	std::string const& option_name)
{
	std::size_t const eq = current.find('=');
	if (eq != std::string::npos) return current.substr(eq + 1);

	if (index + 1 >= args.size()) fail(option_name + " requires a value");
	++index;
	return args[index];
}

void set_mode(options& opt, torrent_mode mode, std::string const& option_name)
{
	if (opt.mode != torrent_mode::hybrid && opt.mode != mode)
		fail(option_name + " conflicts with another torrent mode option");
	opt.mode = mode;
}

options parse_args(std::vector<std::string> const& args)
{
	options opt;
	bool stop_options = false;

	for (std::size_t i = 1; i < args.size(); ++i)
	{
		std::string const& arg = args[i];

		if (stop_options || !starts_with_dash(arg) || arg == "-")
		{
			if (!opt.input.empty()) fail("multiple input paths were provided");
			opt.input = arg;
			continue;
		}

		if (arg == "--")
		{
			stop_options = true;
			continue;
		}
		if (arg == "-h" || arg == "--help")
		{
			print_usage(std::cout);
			throw cli_exit{0};
		}
		if (arg == "--version")
		{
			std::cout << "mktorrent 1.0\n"
			          << "libtorrent " << lt::version() << '\n';
			throw cli_exit{0};
		}

		std::string const name = arg.substr(0, arg.find('='));

		if (name == "-i" || name == "--input")
		{
			if (!opt.input.empty()) fail("multiple input paths were provided");
			opt.input = take_value(args, i, arg, name);
		}
		else if (name == "-o" || name == "--output")
		{
			opt.output = take_value(args, i, arg, name);
			opt.output_set = true;
			opt.write_torrent = true;
		}
		else if (arg == "-m" || arg == "--magnet")
		{
			opt.print_magnet = true;
		}
		else if (name == "-t" || name == "--tracker")
		{
			opt.trackers.push_back(take_value(args, i, arg, name));
		}
		else if (name == "--web-seed")
		{
			opt.url_seeds.push_back(take_value(args, i, arg, name));
		}
		else if (name == "--http-seed")
		{
			opt.http_seeds.push_back(take_value(args, i, arg, name));
		}
		else if (name == "--dht-node")
		{
			opt.dht_nodes.push_back(parse_dht_node(take_value(args, i, arg, name)));
		}
		else if (name == "--comment")
		{
			opt.comment = take_value(args, i, arg, name);
		}
		else if (name == "--creator")
		{
			opt.creator = take_value(args, i, arg, name);
		}
		else if (arg == "--private")
		{
			opt.private_torrent = true;
		}
		else if (name == "--piece-size")
		{
			opt.piece_size = parse_piece_size(take_value(args, i, arg, name));
		}
		else if (arg == "--v1-only")
		{
			set_mode(opt, torrent_mode::v1_only, arg);
		}
		else if (arg == "--v2-only")
		{
			set_mode(opt, torrent_mode::v2_only, arg);
		}
		else if (arg == "--hybrid")
		{
			set_mode(opt, torrent_mode::hybrid, arg);
		}
		else if (arg == "--no-attributes")
		{
			opt.no_attributes = true;
		}
		else if (arg == "--mtime")
		{
			opt.include_mtime = true;
		}
		else if (arg == "--symlinks")
		{
			opt.preserve_symlinks = true;
		}
		else if (arg == "-q" || arg == "--quiet")
		{
			opt.quiet = true;
		}
		else
		{
			fail("unknown option: " + arg);
		}
	}

	if (opt.input.empty()) fail("missing input path; run with --help for usage");
	if (!opt.output_set && !opt.print_magnet) fail("--magnet or --output must be specified");

	return opt;
}

fs::path absolute_path(fs::path const& path)
{
	fs::path input = path;
	while (!input.empty() && input.filename().empty() && input != input.root_path())
	{
		input = input.parent_path();
	}

	std::error_code ec;
	fs::path result = fs::absolute(input, ec);
	if (ec) fail("failed to resolve path '" + path_to_utf8(input) + "': " + ec.message());
	return result;
}

void require_existing_input(fs::path const& path)
{
	std::error_code ec;
	bool const exists = fs::exists(path, ec);
	if (ec) fail("failed to inspect input path '" + path_to_utf8(path) + "': " + ec.message());
	if (!exists) fail("input path does not exist: " + path_to_utf8(path));
}

lt::create_flags_t torrent_flags(options const& opt)
{
	lt::create_flags_t flags;
	if (opt.mode == torrent_mode::v1_only) flags |= lt::create_torrent::v1_only;
	if (opt.mode == torrent_mode::v2_only) flags |= lt::create_torrent::v2_only;
	if (opt.no_attributes) flags |= lt::create_torrent::no_attributes;
	if (opt.include_mtime) flags |= lt::create_torrent::modification_time;
	if (opt.preserve_symlinks) flags |= lt::create_torrent::symlinks;
	return flags;
}

std::vector<char> create_metadata(options const& opt, fs::path const& input_path)
{
	fs::path const abs_input = absolute_path(input_path);
	fs::path const base_path = abs_input.parent_path().empty() ? fs::current_path() : abs_input.parent_path();
	std::string const input_utf8 = path_to_utf8(abs_input);
	std::string const base_utf8 = path_to_utf8(base_path);

	lt::create_flags_t const flags = torrent_flags(opt);

	lt::file_storage storage;
	lt::add_files(storage, input_utf8, flags);
	if (storage.num_files() == 0) fail("no files were added from input path: " + input_utf8);

	lt::create_torrent torrent(storage, opt.piece_size, flags);
	torrent.set_creator(opt.creator.c_str());
	if (!opt.comment.empty()) torrent.set_comment(opt.comment.c_str());
	torrent.set_priv(opt.private_torrent);

	for (std::string const& tracker : opt.trackers) torrent.add_tracker(tracker);
	for (std::string const& seed : opt.url_seeds) torrent.add_url_seed(seed);
	for (std::string const& seed : opt.http_seeds) torrent.add_http_seed(seed);
	for (auto const& node : opt.dht_nodes) torrent.add_node(node);

	if (!opt.quiet)
	{
		std::cerr << "Files: " << storage.num_files()
		          << ", size: " << format_bytes(storage.total_size())
		          << ", pieces: " << torrent.num_pieces()
		          << ", piece size: " << format_bytes(torrent.piece_length())
		          << '\n';
	}

	lt::error_code ec;
	int next_report = 0;
	int const total_pieces = std::max(1, torrent.num_pieces());
	auto progress = [&](lt::piece_index_t piece) {
		if (opt.quiet) return;
		int const done = std::min(total_pieces, static_cast<int>(piece) + 1);
		int const percent = done * 100 / total_pieces;
		if (percent >= next_report || done == total_pieces)
		{
			std::cerr << "\rHashing: " << percent << "% (" << done << "/" << total_pieces << " pieces)" << std::flush;
			next_report = percent + 5;
		}
	};

	lt::set_piece_hashes(torrent, base_utf8, progress, ec);
	if (!opt.quiet) std::cerr << "\rHashing: 100% (" << total_pieces << "/" << total_pieces << " pieces)\n";
	if (ec) fail("failed to hash input files: " + ec.message());

	return torrent.generate_buf();
}

void write_file(fs::path const& path, std::vector<char> const& data)
{
	std::ofstream out(path, std::ios::binary);
	if (!out) fail("failed to open output file: " + path_to_utf8(path));

	out.write(data.data(), static_cast<std::streamsize>(data.size()));
	if (!out) fail("failed to write output file: " + path_to_utf8(path));
}

std::string make_magnet(std::vector<char> const& metadata)
{
	if (metadata.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		fail("generated torrent metadata is too large to build a magnet link");

	lt::error_code ec;
	lt::torrent_info info(metadata.data(), static_cast<int>(metadata.size()), ec);
	if (ec) fail("failed to parse generated torrent metadata: " + ec.message());

	std::string magnet = lt::make_magnet_uri(info);
	if (magnet.empty()) fail("libtorrent did not return a magnet URI");
	return magnet;
}

int run(std::vector<std::string> const& args)
{
	options opt = parse_args(args);

	fs::path const input_path = make_path(opt.input);
	require_existing_input(input_path);

	std::vector<char> metadata = create_metadata(opt, input_path);

	if (opt.write_torrent)
	{
		fs::path const output_path = make_path(opt.output);
		write_file(output_path, metadata);
		if (!opt.quiet)
			std::cerr << "Wrote " << path_to_utf8(output_path) << " (" << format_bytes(static_cast<std::int64_t>(metadata.size())) << ")\n";
	}

	if (opt.print_magnet) std::cout << make_magnet(metadata) << '\n';

	return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
	try
	{
		std::vector<std::string> args;
		args.reserve(static_cast<std::size_t>(argc));
		for (int i = 0; i < argc; ++i) args.push_back(wide_to_utf8(argv[i]));
		return run(args);
	}
	catch (cli_exit const& exit)
	{
		return exit.code;
	}
	catch (std::exception const& e)
	{
		std::cerr << "error: " << e.what() << '\n';
		return 1;
	}
}
#else
int main(int argc, char* argv[])
{
	try
	{
		std::vector<std::string> args(argv, argv + argc);
		return run(args);
	}
	catch (cli_exit const& exit)
	{
		return exit.code;
	}
	catch (std::exception const& e)
	{
		std::cerr << "error: " << e.what() << '\n';
		return 1;
	}
}
#endif


//
// executable.cpp
// Copyright (C) 2015 Félix Cloutier.
// All Rights Reserved.
//
// This file is part of fcd. fcd as a whole is licensed under the terms
// of the GNU GPLv3 license, but specific parts (such as this one) are
// dual-licensed under the terms of a BSD-like license as well. You
// may use, modify and distribute this part of fcd under the terms of
// either license, at your choice. See the LICENSE file in this directory
// for details.
//

#include "command_line.h"
#include "executable.h"
#include "executable_errors.h"
#include "elf_executable.h"
#include "flat_binary.h"
#include "python_executable.h"

#include <ctype.h>

using namespace llvm;
using namespace std;

namespace
{
	// http://stackoverflow.com/a/2886589/251153
	// "all you have to do"... understatement of the week!
	struct ci_char_traits : public char_traits<char>
	{
		static bool eq(char c1, char c2) { return toupper(c1) == toupper(c2); }
		static bool ne(char c1, char c2) { return toupper(c1) != toupper(c2); }
		static bool lt(char c1, char c2) { return toupper(c1) <  toupper(c2); }
		
		static int compare(const char* s1, const char* s2, size_t n)
		{
			while (n != 0)
			{
				--n;
				if (toupper(*s1) < toupper(*s2))
				{
					return -1;
				}
				if (toupper(*s1) > toupper(*s2))
				{
					return 1;
				}
				++s1;
				++s2;
			}
			return 0;
		}
		
		static const char* find(const char* s, int n, char a)
		{
			while (n > 0 && !eq(*s, a))
			{
				--n;
				++s;
			}
			return s;
		}
	};
	
	typedef basic_string<char, ci_char_traits> ci_string;
	
	class AutoExecutableFactory : public ExecutableFactory
	{
		static const char elf_magic[4];
		
	public:
		AutoExecutableFactory()
		: ExecutableFactory("auto", "autodetect")
		{
		}
		
		virtual llvm::ErrorOr<std::unique_ptr<Executable>> parse(const uint8_t* begin, const uint8_t* end) override
		{
			if (end - begin >= sizeof elf_magic && memcmp(begin, elf_magic, sizeof elf_magic) == 0)
			{
				return ElfExecutableFactory().parse(begin, end);
			}
			
			return make_error_code(ExecutableParsingError::Generic_UnknownFormat);
		}
	};
	
	const char AutoExecutableFactory::elf_magic[4] = {0x7f, 'E', 'L', 'F'};
	
	AutoExecutableFactory autoFactory;
	ElfExecutableFactory elfFactory;
	FlatBinaryExecutableFactory flatBinaryFactory;
	PythonExecutableFactory pythonScriptExecutableFactory;
	
	class ExecutableFactoryParser : public cl::generic_parser_base
	{
		struct OptionInfo : public GenericOptionInfo
		{
			cl::OptionValue<ExecutableFactory*> factory;
			
			OptionInfo(ExecutableFactory* factory)
			: GenericOptionInfo(factory->getParameterValue().c_str(), factory->getHelp().c_str()), factory(factory)
			{
			}
		};
		
		static vector<OptionInfo>& factories()
		{
			static vector<OptionInfo> factories = {
				OptionInfo(&autoFactory),
				OptionInfo(&elfFactory),
				OptionInfo(&flatBinaryFactory),
				OptionInfo(&pythonScriptExecutableFactory)
			};
			return factories;
		}
		
	public:
		typedef ExecutableFactory* parser_data_type;
		
		ExecutableFactoryParser(cl::Option& o)
		: cl::generic_parser_base(o)
		{
		}
		
		virtual unsigned getNumOptions() const override
		{
			return static_cast<unsigned>(factories().size());
		}
		
		virtual const char* getOption(unsigned n) const override
		{
			return factories().at(n).Name;
		}
		
		virtual const char* getDescription(unsigned n) const override
		{
			return factories().at(n).HelpStr;
		}
		
		virtual const cl::GenericOptionValue& getOptionValue(unsigned n) const override
		{
			return factories().at(n).factory;
		}
		
		bool parse(cl::Option& o, StringRef argName, StringRef arg, ExecutableFactory*& value)
		{
			StringRef argVal = Owner.hasArgStr() ? arg : argName;
			ci_string ciArgVal(argVal.begin(), argVal.end());
			for (const auto& info : factories())
			{
				if (ciArgVal == info.Name)
				{
					value = info.factory.getValue();
					return false;
				}
			}
			
			if (ciArgVal.length() > 3 && ciArgVal.compare(ciArgVal.length() - 3, 3, ".py") == 0)
			{
				pythonScriptExecutableFactory.setScriptPath(argVal);
				value = &pythonScriptExecutableFactory;
				return false;
			}
			
			return o.error("Cannot find option named '" + argVal + "'!");
		}
	};
	
	cl::opt<ExecutableFactory*, false, ExecutableFactoryParser> executableFactory(
		"format", cl::value_desc("format"),
		cl::desc("Executable format"),
		cl::init(&autoFactory),
		whitelist()
	);
	
	cl::alias formatA("f", cl::desc("Alias for --format"), cl::aliasopt(executableFactory), whitelist());
}

vector<uint64_t> Executable::getVisibleEntryPoints() const
{
	vector<uint64_t> result;
	for (const auto& pair : symbols)
	{
		result.push_back(pair.second.virtualAddress);
	}
	return result;
}

const SymbolInfo* Executable::getInfo(uint64_t address) const
{
	auto iter = symbols.find(address);
	if (iter != symbols.end())
	{
		return &iter->second;
	}
	else if (const uint8_t* memory = map(address))
	{
		SymbolInfo& info = symbols[address];
		info.virtualAddress = address;
		info.memory = memory;
		return &info;
	}
	return nullptr;
}

const StubInfo* Executable::getStubTarget(uint64_t address) const
{
	auto iter = stubTargets.find(address);
	if (iter != stubTargets.end())
	{
		return &iter->second;
	}
	
	string libraryName;
	string targetName;
	switch (doGetStubTarget(address, libraryName, targetName))
	{
		case ResolvedInFlatNamespace:
		{
			StubInfo& stub = stubTargets[address];
			stub.sharedObject = nullptr;
			stub.name = move(targetName);
			return &stub;
		}
		case ResolvedInTwoLevelNamespace:
		{
			auto libIter = libraries.insert(libraryName).first;
			StubInfo& stub = stubTargets[address];
			stub.sharedObject = &*libIter;
			stub.name = move(targetName);
			return &stub;
		}
		case Unresolved:
			return nullptr;
		default:
			llvm_unreachable("Unknown stub target resolution type!");
	}
}

ErrorOr<unique_ptr<Executable>> Executable::parse(const uint8_t* begin, const uint8_t* end)
{
	return executableFactory->parse(begin, end);
}

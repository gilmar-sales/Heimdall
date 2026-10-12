#include <Heimdall/IndexRename.hpp>
#include <Heimdall/ShardBuilder.hpp>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

namespace
{
    namespace idx = heimdall::index;

    struct Project
    {
        std::map<std::string, std::string> files;
        std::shared_ptr<const idx::IndexSnapshot> snapshot;
        heimdall::IndexRenameContext context;

        explicit Project(std::map<std::string, std::string> sources) : files(std::move(sources))
        {
            std::vector<std::shared_ptr<const idx::FileShard>> shards;
            for (const auto& [path, text] : files)
            {
                const auto tree = heimdall::ParseTree::Parse(text);
                const auto model = heimdall::Binder::Bind(tree);
                shards.push_back(idx::ShardBuilder::Build(model, path));
            }

            snapshot = idx::IndexSnapshot::Build(std::move(shards));
            context.complete = true;
            context.document =[this](const std::string& path)->std::optional<heimdall::IndexedDocument>
            {
                const auto found = files.find(path);
                if (found == files.end())
                {
                    return std::nullopt;
                }

                heimdall::IndexedDocument document;
                document.text = std::make_shared<const std::string>(found->second);
                return document;
            };
            context.editable =[](const std::string&)
            {
                return true;
            };
        }

        std::size_t At(const std::string& path, std::string_view needle, std::size_t nth = 0) const
        {
            std::size_t at = 0;
            for (std::size_t i = 0; i <= nth; ++i)
            {
                at = files.at(path).find(needle, i == 0 ? 0 : at + 1);
            }

            return at;
        }

        auto Rename(const std::string& path, std::size_t offset, std::string_view name)
        {
            return heimdall::IndexRename::Rename(*snapshot, path, offset, name, context);
        }

        // Applies a plan to the in-memory files.
        std::map<std::string, std::string> Apply(const heimdall::RefactoringPlan& plan) const
        {
            auto result = files;
            for (const auto& document : plan.documents)
            {
                auto& text = result[document.path];
                for (auto edit = document.edits.rbegin(); edit != document.edits.rend(); ++edit)
                {
                    text.replace(edit->offset, edit->length, edit->replacement);
                }
            }

            return result;
        }
    };
} // namespace

TEST(IndexRename, ClassIsRenamedEverywhereIncludingConstructorsAndQualifiers)
{
    Project project({
            {
                "widget.hpp",
                "#pragma once\nstruct Widget;\nstruct Widget\n{\n    Widget();\n    ~Widget();\n    int Size() const;\n};\n"
        },
            {"widget.cpp",
                "#include \"widget.hpp\"\nWidget::Widget() {}\nWidget::~Widget() {}\nint Widget::Size() const { return 1; }\n"},
            {"use.cpp", "#include \"widget.hpp\"\nint f(const Widget& w) { Widget local; return w.Size(); }\n"},
    });

    const auto plan = project.Rename("widget.hpp", project.At("widget.hpp", "Widget", 1), "Gadget");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto result = project.Apply(*plan);
    EXPECT_EQ(result.at("widget.hpp"),
              "#pragma once\nstruct Gadget;\nstruct Gadget\n{\n    Gadget();\n    ~Gadget();\n    int Size() const;\n};\n");
    EXPECT_EQ(result.at("widget.cpp"),
              "#include \"widget.hpp\"\nGadget::Gadget() {}\nGadget::~Gadget() {}\nint Gadget::Size() const { return 1; }\n");
    EXPECT_EQ(result.at("use.cpp"),
              "#include \"widget.hpp\"\nint f(const Gadget& w) { Gadget local; return w.Size(); }\n");
}

TEST(IndexRename, MethodIsRenamedAtEveryCallAndDefinition)
{
    Project project({
            {
                "a.hpp", "struct A { int Size() const; };\n"
        },
            {"a.cpp", "#include \"a.hpp\"\nint A::Size() const { return 0; }\n"},
            {"use.cpp", "#include \"a.hpp\"\nint f(A& a) { return a.Size(); }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Size"), "Count");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto result = project.Apply(*plan);
    EXPECT_EQ(result.at("a.cpp"), "#include \"a.hpp\"\nint A::Count() const { return 0; }\n");
    EXPECT_EQ(result.at("use.cpp"), "#include \"a.hpp\"\nint f(A& a) { return a.Count(); }\n");
}

TEST(IndexRename, OnlyTheSelectedOverloadIsRenamed)
{
    Project project({
            {
                "h.hpp", "int Add(int);\nint Add(int, int);\n"
        },
            {"use.cpp", "#include \"h.hpp\"\nint f() { return Add(1) + Add(1, 2); }\n"},
    });

    const auto plan = project.Rename("h.hpp", project.At("h.hpp", "Add", 1), "Sum");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto result = project.Apply(*plan);
    EXPECT_EQ(result.at("h.hpp"), "int Add(int);\nint Sum(int, int);\n");
    EXPECT_EQ(result.at("use.cpp"), "#include \"h.hpp\"\nint f() { return Add(1) + Sum(1, 2); }\n");
}

TEST(IndexRename, SameNameInAnotherNamespaceIsLeftAlone)
{
    Project project({
            {
                "a.hpp", "namespace a { struct Item {}; }\n"
        },
            {"b.hpp", "namespace b { struct Item {}; }\n"},
            {"use.cpp", "#include \"a.hpp\"\n#include \"b.hpp\"\na::Item x;\nb::Item y;\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Item"), "Thing");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto result = project.Apply(*plan);
    EXPECT_EQ(result.at("b.hpp"), "namespace b { struct Item {}; }\n");
    EXPECT_EQ(result.at("use.cpp"),
              "#include \"a.hpp\"\n#include \"b.hpp\"\na::Thing x;\nb::Item y;\n");
}

TEST(IndexRename, UsingDeclarationsAndFriendNamesAreRenamed)
{
    Project project({
            {
                "a.hpp", "namespace a { struct B {}; }\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nusing a::B;\nstruct C { friend struct B; };\nB value;\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "B"), "Boxed");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto result = project.Apply(*plan);
    EXPECT_EQ(result.at("use.cpp"),
              "#include \"a.hpp\"\nusing a::Boxed;\nstruct C { friend struct Boxed; };\nBoxed value;\n");
}

TEST(IndexRename, RefusesWhileTheIndexIsIncomplete)
{
    Project project({
            {
                "a.hpp", "struct A {};\n"
            }, {"use.cpp", "#include \"a.hpp\"\nA a;\n"}});
    project.context.complete = false;

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "A"), "B");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::IncompleteAnalysis);
}

TEST(IndexRename, RefusesWhenAnotherFileUsesTheSpellingWithoutAResolvableDeclaration)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nint f() { return Compute(); }\n"},
            {"loose.cpp", "int g() { return Compute(); }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::IncompleteAnalysis);
    EXPECT_NE(plan.error().message.find("loose.cpp"), std::string::npos);
}

TEST(IndexRename, RefusesWhenAMemberCallCouldBelongToAnotherClass)
{
    Project project({
            {
                "a.hpp", "struct A { int Size(); };\nstruct B { int Size(); };\n"
        },
            {"use.cpp", "#include \"a.hpp\"\ntemplate <class T> int f(T& t) { return t.Size(); }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Size"), "Count");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code,
              heimdall::RefactoringErrorCode::IncompleteAnalysis) << plan.error().message;
}

TEST(IndexRename, RefusesWhenTheNameAppearsInAMacro)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"use.cpp", "#include \"a.hpp\"\n#define CALL Compute()\nint f() { return CALL; }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::MacroContext);
}

TEST(IndexRename, RenamesVirtualFunctionsAsACompleteFamily)
{
    Project project({
            {
                "a.hpp", "struct A { virtual int Run(); };\nstruct B : A { int Run() override; };\n"
        },
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Run"), "Execute");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(project.Apply(*plan).at("a.hpp"), "struct A { virtual int Execute(); };\nstruct B : A { int Execute() override; };\n");
}

TEST(IndexRename, RefusesToMergeWithAnExistingDeclaration)
{
    Project project({
            {
                "a.hpp", "int First(int);\nint Second(int);\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nint f() { return First(1); }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "First"), "Second");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::NameCollision);
}

TEST(IndexRename, RefusesWhenTheNewNameCapturesAnInnerUse)
{
    Project project({
            {
                "a.hpp", "int Outer(int);\nnamespace n { int Inner(int); }\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nnamespace n { int f() { return Outer(1) + Inner(2); } }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Outer"), "Inner");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code,
              heimdall::RefactoringErrorCode::SemanticChange) << plan.error().message;
}

TEST(IndexRename, RefusesInvalidAndKeywordNames)
{
    Project project({
            {
                "a.hpp", "struct A {};\n"
            }, {"use.cpp", "#include \"a.hpp\"\nA a;\n"}});

    EXPECT_EQ(project.Rename("a.hpp", project.At("a.hpp", "A"), "class").error().code,
              heimdall::RefactoringErrorCode::InvalidName);
    EXPECT_EQ(project.Rename("a.hpp", project.At("a.hpp", "A"), "1x").error().code,
              heimdall::RefactoringErrorCode::InvalidName);
}

TEST(IndexRename, RefusesWhenTheIndexedTextIsOutdated)
{
    Project project({
            {
                "a.hpp", "struct A {};\n"
            }, {"use.cpp", "#include \"a.hpp\"\nA a;\n"}});
    const auto offset = project.At("a.hpp", "A");
    project.files["use.cpp"] += "// edited\n";

    const auto plan = project.Rename("a.hpp", offset, "B");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::StaleSnapshot);
}

TEST(IndexRename, RefusesFilesOutsideTheWorkspace)
{
    Project project({
            {
                "a.hpp", "struct A {};\n"
            }, {"use.cpp", "#include \"a.hpp\"\nA a;\n"}});
    project.context.editable =[](const std::string& path)
    {
        return path != "a.hpp";
    };

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "A"), "B");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code,
              heimdall::RefactoringErrorCode::UnsupportedSymbol) << plan.error().message;
}

TEST(IndexRename, SpansFilesIsTrueOnlyForEntitiesUsedElsewhere)
{
    Project project({
            {
                "a.hpp", "struct A {};\nstruct Alone {};\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nA a;\n"},
    });

    EXPECT_TRUE(heimdall::IndexRename::SpansFiles(*project.snapshot, "a.hpp",
                                                  project.At("a.hpp", "A")));
    EXPECT_FALSE(heimdall::IndexRename::SpansFiles(*project.snapshot, "a.hpp",
                                                   project.At("a.hpp", "Alone")));
}

TEST(IndexRename, RefusesAnUnknownReceiverEvenWithOneVisibleMember)
{
    Project project({
            {
                "a.hpp", "struct A { int Size(); };\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nint f(Unknown& object) { return object.Size(); }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Size"), "Count");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::IncompleteAnalysis);
}

TEST(IndexRename, PreservesIndependentLocalHomonyms)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nint f() { return Compute(); }\n"},
            {"local.cpp", "int g(int Compute) { return Compute; }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(project.Apply(*plan).at("local.cpp"), project.files.at("local.cpp"));
}

TEST(IndexRename, RefusesReservedAndAlternativeOperatorNames)
{
    Project project({
            {
                "a.hpp", "struct A {};\n"
    }});

    for (const auto name :
        {
            "_Name", "name__part", "and", "not_eq"
    })
    {
        const auto plan = project.Rename("a.hpp", project.At("a.hpp", "A"), name);

        ASSERT_FALSE(plan.has_value()) << name;
        EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::InvalidName);
    }
}

TEST(IndexRename, RefusesWhenTheNewNameIsAMacro)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"use.cpp", "#include \"a.hpp\"\n#define Evaluate replacement\nint f() { return Compute(); }\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::MacroContext);
}

TEST(IndexRename, CancellationTakesPrecedenceOverIncompleteCoverage)
{
    Project project({
            {
                "a.hpp", "struct A {};\n"
    }});
    project.context.complete = false;
    std::stop_source stop;
    stop.request_stop();

    const auto plan = heimdall::IndexRename::Rename(*project.snapshot, "a.hpp",
        project.At("a.hpp", "A"), "B", project.context, stop.get_token());

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::Cancelled);
}

TEST(IndexRename, RefusesExistingParseErrorsInAnEditedFile)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nint f() { return Compute(); }\nint broken(\n"},
    });

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::IncompleteAnalysis);
}

TEST(IndexRename, RefusesPredefinedMacroAsNewName)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto load = project.context.document;
    project.context.document =[load](const std::string& path)
    {
        auto document = load(path);
        document->options.emplace();
        document->options->predefined_macros.emplace("Evaluate", "replacement");
        return document;
    };

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::MacroContext);
}

TEST(IndexRename, PreviewSupportsClosedFilesWithoutWorkspaceDocumentIds)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"use.cpp", "#include \"a.hpp\"\nint f() { return Compute(); }\n"},
    });
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context);

    ASSERT_TRUE(preview.has_value()) << preview.error().message;
    ASSERT_EQ(preview->size(), 2u);
    const auto expected = project.Apply(*plan);
    for (const auto& document : *preview)
    {
        EXPECT_EQ(document.source, expected.at(document.path.generic_string()));
    }

    EXPECT_EQ(project.files.at("a.hpp"), "int Compute();\n");
}

TEST(IndexRename, PreviewRejectsChangesToUneditedProofDocuments)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
        },
            {"other.cpp", "int Other();\n"},
    });
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    project.files["other.cpp"] += "int Evaluate();\n";

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context);

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::StaleSnapshot);
}

TEST(IndexRename, PreviewRejectsChangedDocumentVersions)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto load = project.context.document;
    project.context.document =[load](const std::string& path)
    {
        auto document = load(path);
        document->version = 2;
        return document;
    };

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context);

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::StaleSnapshot);
}

TEST(IndexRename, PreviewRejectsChangedParserOptions)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto load = project.context.document;
    project.context.document =[load](const std::string& path)
    {
        auto document = load(path);
        document->options.emplace();
        return document;
    };
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    project.context.document =[load](const std::string& path)
    {
        auto document = load(path);
        document->options.emplace();
        document->options->standard = heimdall::CppStandard::Cpp26;
        return document;
    };

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context);

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::StaleSnapshot);
}

TEST(IndexRename, PreviewRejectsANewerIndexGeneration)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    const auto current = idx::IndexSnapshot::Build(
        {project.snapshot->Shards().begin(), project.snapshot->Shards().end()}, 1);

    const auto preview = heimdall::IndexRename::Preview(*current, *plan, project.context);

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::StaleSnapshot);
}

TEST(IndexRename, PreviewRejectsOverlappingEdits)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    plan->documents.front().edits.push_back(plan->documents.front().edits.front());

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context);

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::ConflictingEdits);
}

TEST(IndexRename, RefusesAnOutdatedConfigurationFingerprint)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto load = project.context.document;
    project.context.document =[load](const std::string& path)
    {
        auto document = load(path);
        document->configHash = 1;
        return document;
    };

    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::StaleSnapshot);
}

TEST(IndexRename, PreviewRejectsDuplicateDocumentPaths)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    plan->documents.push_back(plan->documents.front());

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context);

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::ConflictingEdits);
}

TEST(IndexRename, PreviewRejectsInvalidEditBoundsAndExpectedBytes)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto wrongBytes = *plan;
    wrongBytes.documents.front().edits.front().expected = "Wrongxx";
    auto wrongBounds = *plan;
    wrongBounds.documents.front().edits.front().offset = project.files.at("a.hpp").size() + 1;

    const auto bytes = heimdall::IndexRename::Preview(*project.snapshot, wrongBytes, project.context);
    const auto bounds = heimdall::IndexRename::Preview(*project.snapshot, wrongBounds, project.context);

    ASSERT_FALSE(bytes.has_value());
    ASSERT_FALSE(bounds.has_value());
    EXPECT_EQ(bytes.error().code, heimdall::RefactoringErrorCode::InvalidEdit);
    EXPECT_EQ(bounds.error().code, heimdall::RefactoringErrorCode::InvalidEdit);
}

TEST(IndexRename, PreviewHonorsPreRequestedCancellation)
{
    Project project({
            {
                "a.hpp", "int Compute();\n"
    }});
    const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    project.context.complete = false;
    std::stop_source stop;
    stop.request_stop();

    const auto preview = heimdall::IndexRename::Preview(*project.snapshot, *plan, project.context,
        stop.get_token());

    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, heimdall::RefactoringErrorCode::Cancelled);
}

TEST(IndexRename, PrepareAndRenameShareTheConstructorClassTarget)
{
	Project project({
		{"a.hpp", "struct A { A(); ~A(); };\n"},
		{"a.cpp", "#include \"a.hpp\"\nA::A() {}\nA::~A() {}\n"},
	});
	const auto offset = project.At("a.hpp", "A", 1);

	const auto prepared = heimdall::IndexRename::Prepare(*project.snapshot, "a.hpp", offset, project.context);
	const auto renamed = project.Rename("a.hpp", offset, "B");

	ASSERT_TRUE(prepared.has_value()) << prepared.error().message;
	ASSERT_TRUE(renamed.has_value()) << renamed.error().message;
	EXPECT_EQ(prepared->name, "A");
	EXPECT_EQ(prepared->offset, offset);
	EXPECT_EQ(project.Apply(*renamed).at("a.cpp"), "#include \"a.hpp\"\nB::B() {}\nB::~B() {}\n");
}

TEST(IndexRename, RenamesNamespaceReopeningsAndContainedQualifiers)
{
	Project project({
		{"a.hpp", "namespace original { struct A {}; int Run(); }\n"},
		{"a.cpp", "#include \"a.hpp\"\nnamespace original { int Run() { return 0; } }\n"},
		{"use.cpp", "#include \"a.hpp\"\noriginal::A object;\nint f() { return original::Run(); }\n"},
	});

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "original"), "renamed");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	const auto result = project.Apply(*plan);
	EXPECT_EQ(result.at("a.cpp"), "#include \"a.hpp\"\nnamespace renamed { int Run() { return 0; } }\n");
	EXPECT_EQ(result.at("use.cpp"), "#include \"a.hpp\"\nrenamed::A object;\nint f() { return renamed::Run(); }\n");
}

TEST(IndexRename, ExactIncludeVisibilityIsPreservedDuringSpeculativeReindexing)
{
	Project project({
		{"first/widget.hpp", "namespace first { struct Widget {}; }\n"},
		{"second/widget.hpp", "namespace second { struct Widget {}; }\n"},
		{"use.cpp", "#include \"widget.hpp\"\nsecond::Widget value;\n"},
	});
	std::vector<std::shared_ptr<const idx::FileShard>> shards;
	for (const auto& [path, text] : project.files)
	{
		const auto tree = heimdall::ParseTree::Parse(text);
		const auto model = heimdall::Binder::Bind(tree);
		shards.push_back(idx::ShardBuilder::Build(model, path, 0,
			[](std::string_view, bool, std::uint32_t) -> std::optional<std::string>
			{
				return "second/widget.hpp";
			}));
	}
	project.snapshot = idx::IndexSnapshot::Build(std::move(shards));
	project.context.requireExactIncludes = true;

	const auto plan = project.Rename("second/widget.hpp", project.At("second/widget.hpp", "Widget"), "Gadget");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	const auto result = project.Apply(*plan);
	EXPECT_EQ(result.at("first/widget.hpp"), project.files.at("first/widget.hpp"));
	EXPECT_EQ(result.at("use.cpp"), "#include \"widget.hpp\"\nsecond::Gadget value;\n");
}

TEST(IndexRename, ExactModeIgnoresUnresolvedIncludesInInactiveBranches)
{
	Project project({
		{"a.hpp", "int Compute();\n"},
		{"use.cpp", "#include \"a.hpp\"\n#if defined(_WIN32)\n#include <windows.h>\n#endif\nint f() { return Compute(); }\n"},
	});
	std::vector<std::shared_ptr<const idx::FileShard>> shards;
	for (const auto& [path, text] : project.files)
	{
		const auto tree = heimdall::ParseTree::Parse(text);
		const auto model = heimdall::Binder::Bind(tree);
		shards.push_back(idx::ShardBuilder::Build(model, path, 0,
			[](std::string_view spelling, bool, std::uint32_t) -> std::optional<std::string>
			{
				return spelling == "a.hpp" ? std::optional<std::string>("a.hpp") : std::nullopt;
			}));
	}
	project.snapshot = idx::IndexSnapshot::Build(std::move(shards));
	project.context.requireExactIncludes = true;

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
}

TEST(IndexRename, ExactModeIgnoresIncludeTargetsOutsideTheEditableWorkspace)
{
	Project project({
		{"a.hpp", "int Compute();\n"},
		{"use.cpp", "#include \"a.hpp\"\n#include <vector>\nint f() { return Compute(); }\n"},
	});
	std::vector<std::shared_ptr<const idx::FileShard>> shards;
	for (const auto& [path, text] : project.files)
	{
		const auto tree = heimdall::ParseTree::Parse(text);
		const auto model = heimdall::Binder::Bind(tree);
		shards.push_back(idx::ShardBuilder::Build(model, path, 0,
			[](std::string_view spelling, bool, std::uint32_t) -> std::optional<std::string>
			{
				return spelling == "a.hpp" ? "a.hpp" : "/usr/include/c++/16/vector";
			}));
	}
	project.snapshot = idx::IndexSnapshot::Build(std::move(shards));
	project.context.requireExactIncludes = true;
	project.context.editable = [](const std::string& path)
	{
		return !path.starts_with("/usr/");
	};

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
}

TEST(IndexRename, ExactModeRefusesHeuristicIncludeVisibility)
{
	Project project({
		{"a.hpp", "int Compute();\n"},
		{"use.cpp", "#include \"a.hpp\"\nint f() { return Compute(); }\n"},
	});
	project.context.requireExactIncludes = true;

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Compute"), "Evaluate");

	ASSERT_FALSE(plan.has_value());
	EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::IncompleteAnalysis);
}

TEST(IndexRename, RenamesImplicitVirtualOverridesAcrossFilesFromTheDerivedDeclaration)
{
	Project project({
		{"base.hpp", "struct A { virtual int Run(); };\n"},
		{"derived.hpp", "#include \"base.hpp\"\nstruct B : A { int Run(); };\n"},
		{"most.hpp", "#include \"derived.hpp\"\nstruct C : B { int Run(); };\n"},
		{"impl.cpp", "#include \"most.hpp\"\nint A::Run() { return 1; }\nint B::Run() { return 2; }\nint C::Run() { return 3; }\n"},
		{"use.cpp", "#include \"most.hpp\"\nint f(A& a, B& b, C& c) { return a.Run() + b.Run() + c.Run(); }\n"},
	});

	const auto plan = project.Rename("most.hpp", project.At("most.hpp", "Run"), "Execute");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	const auto result = project.Apply(*plan);
	EXPECT_EQ(result.at("base.hpp"), "struct A { virtual int Execute(); };\n");
	EXPECT_EQ(result.at("derived.hpp"), "#include \"base.hpp\"\nstruct B : A { int Execute(); };\n");
	EXPECT_EQ(result.at("use.cpp"), "#include \"most.hpp\"\nint f(A& a, B& b, C& c) { return a.Execute() + b.Execute() + c.Execute(); }\n");
	EXPECT_EQ(result.at("impl.cpp"), "#include \"most.hpp\"\nint A::Execute() { return 1; }\nint B::Execute() { return 2; }\nint C::Execute() { return 3; }\n");
}

TEST(IndexRename, VirtualFamilyLeavesUnrelatedVirtualMethodsUntouched)
{
	Project project({
		{"a.hpp", "struct A { virtual int Run(); };\nstruct B : A { int Run(); };\nstruct Unrelated { virtual int Run(); };\n"},
		{"use.cpp", "#include \"a.hpp\"\nint f(A& a, Unrelated& other) { return a.Run() + other.Run(); }\n"},
	});

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Run"), "Execute");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	EXPECT_EQ(project.Apply(*plan).at("use.cpp"), "#include \"a.hpp\"\nint f(A& a, Unrelated& other) { return a.Execute() + other.Run(); }\n");
}

TEST(IndexRename, VirtualFamilyRefusesNonEditableOverrides)
{
	Project project({
		{"base.hpp", "struct A { virtual int Run(); };\n"},
		{"derived.hpp", "#include \"base.hpp\"\nstruct B : A { int Run(); };\n"},
	});
	project.context.editable = [](const std::string& path) { return path != "derived.hpp"; };

	const auto plan = project.Rename("base.hpp", project.At("base.hpp", "Run"), "Execute");

	ASSERT_FALSE(plan.has_value());
	EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::UnsupportedSymbol);
}

TEST(IndexRename, PreservesDoxygenReferencesAndDeclarationBriefsAcrossFiles)
{
	Project project({
		{"a.hpp", "/** @brief Widget represents a widget. */\nstruct Widget {};\n"},
		{"use.cpp", "#include \"a.hpp\"\n/** @ref Widget */\nWidget value;\n"},
	});

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Widget", 1), "Gadget");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	const auto result = project.Apply(*plan);
	EXPECT_EQ(result.at("a.hpp"), "/** @brief Gadget represents a widget. */\nstruct Gadget {};\n");
	EXPECT_EQ(result.at("use.cpp"), "#include \"a.hpp\"\n/** @ref Gadget */\nGadget value;\n");
}

TEST(IndexRename, RenamesInheritedVirtualCallsAndBareUsesAcrossFiles)
{
	Project project({
		{"base.hpp", "struct Base { virtual int Run(); };\n"},
		{"derived.hpp", "#include \"base.hpp\"\nstruct Derived : Base { int Call(); };\n"},
		{"use.cpp", "#include \"derived.hpp\"\nint Derived::Call() { return Run(); }\nint f(Derived& value) { return value.Run(); }\n"},
	});

	const auto plan = project.Rename("base.hpp", project.At("base.hpp", "Run"), "Execute");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	EXPECT_EQ(project.Apply(*plan).at("use.cpp"), "#include \"derived.hpp\"\nint Derived::Call() { return Execute(); }\nint f(Derived& value) { return value.Execute(); }\n");
}

TEST(IndexRename, RefusesInheritedNewNameCaptureEvenWhenTheLocalBinderBoundAGlobal)
{
	Project project({
		{"base.hpp", "struct Base { virtual int Run(); };\nstruct Derived : Base { int Call(); };\n"},
		{"use.cpp", "#include \"base.hpp\"\nint Helper();\nint Derived::Call() { return Run() + Helper(); }\n"},
	});

	const auto plan = project.Rename("base.hpp", project.At("base.hpp", "Run"), "Helper");

	ASSERT_FALSE(plan.has_value());
	EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::SemanticChange);
}

TEST(IndexRename, UnrelatedSyntaxErrorFixtureDoesNotBlockProjectRename)
{
	Project project({
		{"a.hpp", "struct Widget {};\n"},
		{"test/fixtures/parse_syntax_error.cpp", "int broken(\n"},
	});
	project.context.requireExactIncludes = true;
	const auto invalid = project.snapshot->ShardFor("test/fixtures/parse_syntax_error.cpp");
	ASSERT_FALSE(project.snapshot->Shards()[invalid]->SyntaxComplete());

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Widget"), "Gadget");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	EXPECT_EQ(project.Apply(*plan).at("test/fixtures/parse_syntax_error.cpp"), "int broken(\n");
}

TEST(IndexRename, SyntaxErrorFixtureMentioningTheNewNameBlocksTheProof)
{
	Project project({
		{"a.hpp", "struct Widget {};\n"},
		{"test/fixtures/parse_syntax_error.cpp", "int Gadget(\n"},
	});

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Widget"), "Gadget");

	ASSERT_FALSE(plan.has_value());
	EXPECT_EQ(plan.error().code, heimdall::RefactoringErrorCode::IncompleteAnalysis);
}

TEST(IndexRename, BuiltinArgumentTypesChooseAnExactSameArityOverload)
{
	Project project({
		{"a.hpp", "int Add(long);\nint Add(int);\n"},
		{"use.cpp", "#include \"a.hpp\"\nint f() { return Add(1) + Add(1L); }\n"},
	});

	const auto plan = project.Rename("a.hpp", project.At("a.hpp", "Add", 1), "Sum");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	EXPECT_EQ(project.Apply(*plan).at("a.hpp"), "int Add(long);\nint Sum(int);\n");
	EXPECT_EQ(project.Apply(*plan).at("use.cpp"), "#include \"a.hpp\"\nint f() { return Sum(1) + Add(1L); }\n");
}

TEST(IndexRename, RenamesFunctionCalledInNamespaceScopeVariableInitializer)
{
	Project project({
		{"a.cpp", "int CreateList() { return 1; }\n\nauto list = CreateList();\n"},
	});

	const auto plan = project.Rename("a.cpp", project.At("a.cpp", "CreateList"), "MakeList");

	ASSERT_TRUE(plan.has_value()) << plan.error().message;
	EXPECT_EQ(project.Apply(*plan).at("a.cpp"), "int MakeList() { return 1; }\n\nauto list = MakeList();\n");
}

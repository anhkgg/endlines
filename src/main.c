/*
   This file is part of endlines' source code

   Copyright 2014-2016 Mathias Dolidon

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/


#include "endlines.h"
#include "walkers.h"
#include "file_operations.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>




// =============== LOCAL TYPES  ===============

// Match a convention name as given on the command line to
// the Convention enum type, defined in endlines.h

typedef struct {
    char name[10];
    Convention convention;
} cmd_line_args_to_convention;


// Holds all command line parameters

typedef struct {
    Convention convention;
    bool quiet;
    bool verbose;
    bool binaries;
    bool keepdate;
    bool recurse;
    bool process_hidden;
    char** filenames;
    int file_count;
} CommandLine;




// An accumulator that is passed around by the walkers, to the walkers_callback function
// Its main use is to keep track of what has been done
// It is complemented by the walkers' tracker object, defined in walkers.h,
// that'll hold results that are specific to the walker (e.g. skipped directories and hidden files)

typedef struct {
    int outcome_totals[FILEOP_STATUSES_COUNT];
    int convention_totals[CONVENTIONS_COUNT];
    CommandLine* cmd_line_args;
} Accumulator;




// =============== ALL ABOUT CONVENTION NAMES ===============

#define CL_NAMES_COUNT 11
const cmd_line_args_to_convention cl_names[] = {
    {.name="check",   .convention=NO_CONVENTION},
    {.name="lf",      .convention=LF},
    {.name="unix",    .convention=LF},
    {.name="linux",   .convention=LF},
    {.name="osx",     .convention=LF},
    {.name="crlf",    .convention=CRLF},
    {.name="win",     .convention=CRLF},
    {.name="windows", .convention=CRLF},
    {.name="dos",     .convention=CRLF},
    {.name="cr",      .convention=CR},
    {.name="oldmac",  .convention=CR}
};

Convention
read_convention_from_string(char * name) {
    for(int i=0; i<CL_NAMES_COUNT; ++i) {
        if(!strcmp(cl_names[i].name, name)) {
            return cl_names[i].convention;
        }
    }
    fprintf(stderr, "endlines : unknown action : %s\n", name);
    exit(8);
}

const char* convention_display_names[CONVENTIONS_COUNT];
const char* convention_short_display_names[CONVENTIONS_COUNT];
void
setup_conventions_display_names() {
    convention_display_names[NO_CONVENTION] = "No line ending";
    convention_short_display_names[NO_CONVENTION] = "None";

    convention_display_names[CR] = "Legacy Mac (CR)";
    convention_short_display_names[CR] = "CR";

    convention_display_names[LF] = "Unix (LF)";
    convention_short_display_names[LF] = "LF";

    convention_display_names[CRLF] = "Windows (CR-LF)";
    convention_short_display_names[CRLF] = "CRLF";

    convention_display_names[MIXED] = "Mixed endings";
    convention_short_display_names[MIXED] = "Mixed";
}





// =============== THE HELP AND VERSION SCREENS ===============

void
display_help_and_quit() {
    fprintf(stderr, "\n"
                    "  endlines ACTION [OPTIONS] [FILES]\n\n"

                    "  ACTION can be :\n"
                    "    lf, unix, linux, osx    : convert all endings to LF.\n"
                    "    crlf, windows, win, dos : convert all endings to CR-LF.\n"
                    "    cr, oldmac              : convert all endings to CR.\n"
                    "    check                   : perform a dry run to check current conventions.\n\n"

                    "  If no files are specified, endlines converts from stdin to stdout.\n"
                    "  Supports UTF-8, UTF-16 with BOM, and all major single byte codesets.\n\n"

                    "  General   -q / --quiet    : silence all but the error messages.\n"
                    "            -v / --verbose  : print more about what's going on.\n"
                    "            --version       : print version and license.\n\n"

                    "  Files     -b / --binaries : don't skip binary files.\n"
                    "            -h / --hidden   : process hidden files (/directories) too.\n"
                    "            -k / --keepdate : keep last modified and last access times.\n"
                    "            -r / --recurse  : recurse into directories.\n\n"

                    "  Examples  endlines check *.txt\n"
                    "            endlines linux -k -r aFolder anotherFolder\n\n");
    exit(1);
}


void
display_version_and_quit() {
    fprintf(stderr, "\n   * endlines version %s \n"

                    "   * Copyright 2014-2016 Mathias Dolidon\n\n"

                    "   Licensed under the Apache License, Version 2.0 (the \"License\");\n"
                    "   you may not use this file except in compliance with the License.\n"
                    "   You may obtain a copy of the License at\n\n"

                    "       http://www.apache.org/licenses/LICENSE-2.0\n\n"

                    "   Unless required by applicable law or agreed to in writing, software\n"
                    "   distributed under the License is distributed on an \"AS IS\" BASIS,\n"
                    "   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
                    "   See the License for the specific language governing permissions and\n"
                    "   limitations under the License.\n\n", VERSION);

    exit(1);
}





// =============== PARSING COMMAND LINE OPTIONS ===============
// Yes it's a huge and ugly switch

CommandLine
parse_cmd_line_args(int argc, char** argv) {
    CommandLine cmd_line_args = {.quiet=false, .binaries=false, .keepdate=false, .verbose=false,
    .recurse=false, .process_hidden=false, .filenames=NULL, .file_count=0};

    cmd_line_args.filenames = malloc(argc*sizeof(char*));
    if(cmd_line_args.filenames == NULL) {
        fprintf(stderr, "Can't allocate memory\n");
        exit(1);
    }

    for(int i=1; i<argc; ++i) {
        if(i>1 && argv[i][0] != '-') {
            cmd_line_args.filenames[cmd_line_args.file_count] = argv[i];
            ++ cmd_line_args.file_count;
        } else if(!strcmp(argv[i], "--help")) {
            display_help_and_quit();
        } else if(!strcmp(argv[i], "--version")) {
            display_version_and_quit();
        } else if(i==1) {
            cmd_line_args.convention = read_convention_from_string(argv[1]);
        } else if(!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            cmd_line_args.quiet = true;
        } else if(!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            cmd_line_args.verbose = true;
        } else if(!strcmp(argv[i], "-b") || !strcmp(argv[i], "--binaries")) {
            cmd_line_args.binaries = true;
        } else if(!strcmp(argv[i], "-k") || !strcmp(argv[i], "--keepdate")) {
            cmd_line_args.keepdate = true;
        } else if(!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recurse")) {
            cmd_line_args.recurse = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--hidden")) {
            cmd_line_args.process_hidden = true;
        } else {
            fprintf(stderr, "endlines : unknown option : %s\n", argv[i]);
            exit(4);
        }
    }

    if(cmd_line_args.file_count == 0) {
        free(cmd_line_args.filenames);
    }

    return cmd_line_args;
}





// =============== CONVERTING OR CHECKING ONE FILE ===============





// Main conversion (resp. checking) handlers

#define TRY partial_status =
#define CATCH if(partial_status != CAN_CONTINUE) { return partial_status; }
#define CATCH_CLOSE_IN if(partial_status != CAN_CONTINUE) { fclose(in); return partial_status; }

void
initialize_session_tmp_filename(char* session_tmp_filename) {
    struct timespec res;
    clock_gettime(CLOCK_REALTIME, &res);
    int suffix = res.tv_nsec % 9999999;
    sprintf(session_tmp_filename, "%s%d", TMP_FILENAME_BASE, suffix);
}

FileOp_Status
pre_conversion_check(
        FILE* in,
        char* filename,
        FileReport* file_report,
        CommandLine* cmd_line_args) {

    ConversionParameters p = {
        .instream=in,
        .outstream=NULL,
        .dst_convention=cmd_line_args->convention,
        .interrupt_if_not_like_dst_convention=true,
        .interrupt_if_non_text=!cmd_line_args->binaries
    };

    FileReport preliminary_report = convert_stream(p);

    if(preliminary_report.error_during_conversion) {
        fprintf(stderr, "endlines : file access error during preliminary check of %s\n", filename);
        return FILEOP_ERROR;
    }

    if(preliminary_report.contains_non_text_chars && !cmd_line_args->binaries) {
        return SKIPPED_BINARY;
    }
    Convention src_convention = get_source_convention(&preliminary_report);
    if(src_convention == NO_CONVENTION || src_convention == cmd_line_args->convention) {
        memcpy(file_report, &preliminary_report, sizeof(FileReport));
        return DONE;
    }
    return CAN_CONTINUE;
}


FileOp_Status
convert_one_file(
        char* filename,
        struct stat* statinfo,
        CommandLine* cmd_line_args,
        FileReport* file_report) {


    FileOp_Status partial_status;
    FILE *in  = NULL;
    FILE *out = NULL;
    
    static char session_tmp_filename[40] = "";
    char local_tmp_file_name[WALKERS_MAX_PATH_LENGTH];
    struct utimbuf original_file_times = get_file_times(statinfo);

    if(!strlen(session_tmp_filename)) {
        initialize_session_tmp_filename(session_tmp_filename);
    }

    TRY open_input_file_for_conversion(&in, filename); CATCH
    TRY pre_conversion_check(in, filename, file_report, cmd_line_args); CATCH_CLOSE_IN
    rewind(in);
    int tmp_path_err = make_filename_in_same_location(filename, session_tmp_filename, local_tmp_file_name);
    if(tmp_path_err) {
        fclose(in);
        return FILEOP_ERROR;
    }
    TRY open_temporary_file(&out, local_tmp_file_name); CATCH_CLOSE_IN

    ConversionParameters p = {
        .instream=in,
        .outstream=out,
        .dst_convention=cmd_line_args->convention,
        .interrupt_if_not_like_dst_convention=false,
        .interrupt_if_non_text=!cmd_line_args->binaries
    };
    FileReport report = convert_stream(p);

    fclose(in);
    fclose(out);

    if(report.error_during_conversion) {
        fprintf(stderr, "endlines : file access error during conversion of %s\n", filename);
        return FILEOP_ERROR;
    }
    if(report.contains_non_text_chars && !cmd_line_args->binaries) {
        remove(local_tmp_file_name);
        return SKIPPED_BINARY;
    }

    TRY move_temp_file_to_destination(local_tmp_file_name, filename, statinfo); CATCH

    if(cmd_line_args->keepdate) {
        utime(filename, &original_file_times);
    }

    memcpy(file_report, &report, sizeof(FileReport));
    return DONE;
}


FileOp_Status
check_one_file(char* filename, CommandLine* cmd_line_args, FileReport* file_report) {
    FileOp_Status partial_status;
    FILE *in  = NULL;
    TRY open_input_file_for_dry_run(&in, filename); CATCH

    ConversionParameters p = {
        .instream=in,
        .outstream=NULL,
        .dst_convention=NO_CONVENTION,
        .interrupt_if_not_like_dst_convention=false,
        .interrupt_if_non_text=!cmd_line_args->binaries
    };
    FileReport report = convert_stream(p);

    fclose(in);

    if(report.error_during_conversion) {
        fprintf(stderr, "endlines : file access error during check of %s\n", filename);
        return FILEOP_ERROR;
    }
    if(report.contains_non_text_chars && !cmd_line_args->binaries) {
        return SKIPPED_BINARY;
    }

    memcpy(file_report, &report, sizeof(FileReport));
    return DONE;
}

#undef TRY
#undef CATCH





// =============== HANDLING A CONVERSION BATCH ===============


typedef struct {
    bool dry_run;
    int *count_by_convention;  // array
    int done;
    int directories;
    int binaries;
    int hidden;
    int errors;
} Outcome_totals_for_display;

void
print_verbose_file_outcome(char * filename, FileOp_Status outcome, Convention source_convention) {
    switch(outcome) {
        case DONE:
            fprintf(stderr, "endlines : %s -- %s\n",
                    convention_short_display_names[source_convention], filename);
            break;
        case SKIPPED_BINARY:
            fprintf(stderr, "endlines : skipped probable binary %s\n", filename);
            break;
        default: break;
    }
}



void
print_outcome_totals(Outcome_totals_for_display t) {
    fprintf(stderr,  "\nendlines : %i file%s %s", t.done,
            t.done>1?"s":"", t.dry_run?"checked":"converted");

    if(t.done) {
        fprintf(stderr, " %s :\n", t.dry_run?"; found":"from");
        for(int i=0; i<CONVENTIONS_COUNT; ++i) {
            if(t.count_by_convention[i]) {
                fprintf(stderr, "              - %i %s\n",
                        t.count_by_convention[i], convention_display_names[i]);
            }
        }
    } else {
        fprintf(stderr, "\n");
    }

    if(t.directories) {
        fprintf(stderr, "           %i director%s skipped\n",
                t.directories, t.directories>1?"ies":"y");
    }
    if(t.binaries) {
        fprintf(stderr, "           %i binar%s skipped\n",
                t.binaries, t.binaries>1?"ies":"y");
    }
    if(t.hidden) {
        fprintf(stderr, "           %i hidden file%s skipped\n",
                t.hidden, t.hidden>1?"s":"");
    }
    if(t.errors) {
        fprintf(stderr, "           %i error%s\n",
                t.errors, t.errors>1?"s":"");
    }
    fprintf(stderr, "\n");
}

void
walkers_callback(char* filename, struct stat* statinfo, void* p_accumulator) {
    FileOp_Status outcome;
    FileReport file_report;
    Convention source_convention;
    Accumulator* accumulator = (Accumulator*) p_accumulator;

    if(!accumulator->cmd_line_args->binaries &&
            has_known_binary_file_extension(filename)) {
        outcome = SKIPPED_BINARY;
    } else if(accumulator->cmd_line_args->convention == NO_CONVENTION) {
        outcome = check_one_file(filename, accumulator->cmd_line_args, &file_report);
    } else {
        outcome = convert_one_file(filename, statinfo, accumulator->cmd_line_args, &file_report);
    }

    source_convention = get_source_convention(&file_report);
    ++ accumulator->outcome_totals[outcome];
    if(outcome == DONE) {
        ++ accumulator->convention_totals[source_convention];
    }
    if(accumulator->cmd_line_args->verbose) {
        print_verbose_file_outcome(filename, outcome, source_convention);
    }
}

Accumulator
make_accumulator(CommandLine* cmd_line_args) {
    Accumulator a;
    for(int i=0; i<FILEOP_STATUSES_COUNT; ++i) {
        a.outcome_totals[i] = 0;
    }
    for(int i=0; i<CONVENTIONS_COUNT; ++i) {
        a.convention_totals[i] = 0;
    }
    a.cmd_line_args = cmd_line_args;
    return a;
}

Walk_tracker
make_tracker(CommandLine* cmd_line_args, Accumulator* accumulator) {
    Walk_tracker t = make_default_walk_tracker(); // from walkers.h

    t.process_file = &walkers_callback;
    t.accumulator = accumulator;
    t.verbose = cmd_line_args->verbose;
    t.recurse = cmd_line_args->recurse;
    t.skip_hidden = !cmd_line_args->process_hidden;
    return t;
}

void
convert_files(CommandLine* cmd_line_args)  {
    Accumulator accumulator = make_accumulator(cmd_line_args);
    Walk_tracker tracker = make_tracker(cmd_line_args, &accumulator);

    if(!cmd_line_args->quiet) {
        if(cmd_line_args->convention == NO_CONVENTION) {
            fprintf(stderr, "endlines : dry run, scanning files\n");
        } else {
            fprintf(stderr, "endlines : converting files to %s\n",
                    convention_display_names[cmd_line_args->convention]);
        }
    }

    walk_filenames(cmd_line_args->filenames, cmd_line_args->file_count, &tracker);

    if(!cmd_line_args->quiet) {
        Outcome_totals_for_display totals = {
            .dry_run     = (cmd_line_args->convention == NO_CONVENTION),
            .count_by_convention = accumulator.convention_totals,
            .done        = accumulator.outcome_totals[DONE],
            .directories = tracker.skipped_directories_count,
            .binaries    = accumulator.outcome_totals[SKIPPED_BINARY],
            .hidden      = tracker.skipped_hidden_files_count,
            .errors      = accumulator.outcome_totals[FILEOP_ERROR] + tracker.read_errors_count
        };
        print_outcome_totals(totals);
    }
}





// =============== ENTRY POINT ===============

int
main(int argc, char**argv) {
    if(argc <= 1) {
        display_help_and_quit();
    }

    setup_conventions_display_names();
    CommandLine cmd_line_args = parse_cmd_line_args(argc, argv);

    if(cmd_line_args.file_count > 0) {
        convert_files(&cmd_line_args);
    } else {
        if(!cmd_line_args.quiet) {
            if(cmd_line_args.convention == NO_CONVENTION) {
                fprintf(stderr, "endlines : dry run, scanning standard input\n");
            } else {
                fprintf(stderr, "Converting standard input to %s\n",
                        convention_display_names[cmd_line_args.convention]);
            }
        }
        ConversionParameters p = {
            .instream=stdin,
            .outstream=stdout,
            .dst_convention=cmd_line_args.convention,
            .interrupt_if_non_text=false
        };
        convert_stream(p);
    }
    return 0;
}

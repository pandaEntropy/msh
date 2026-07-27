## hsh - The Hollow Shell

*hsh* is a compact UNIX shell with a focus on state rollback.

State rollback is achieved by utilizing overlayfs and adapting the Memento design pattern.

## Installation

*hsh* is a compact shell designed to demonstrate state rollback in UNIX shells, and is thus not completely production-ready. This page will be updated if *hsh* gets a stable release, but for now running it as the primary shell is not recommended.

* Note: *hsh* can only run on Linux version 3.18 or higher

1. To clone the repo run:

    `git clone https://github.com/pandaEntropy/hsh.git`

2. Run `make` and `sudo make install`. Root permissions are required to set setuid permissions on the executable.

## Testing

* *hsh* supports one operator per command, except for pipes which can be chained indefinitely.

* Supported operators are:

    | Symbol  | Name |
    | ------------- |:-------------:|
    | > | Redirection |
    | && | AND |
    | \| | Pipe |
    | \|\| | OR |
    | & | Background Operator |

* State rollback is achieved by running the built-in command `undo`.

* `undo` reverts the changes made by the most recently executed and rollback-supported command.

* `undo` can restore file content, permissions and shell state where applicable.

* `undo` will not do anything if there is nothing to restore.

* The list of built-ins can be viewed by running `help`.

## Technical Overview - V1

> This section is outdated and kept here for educational purposes. Refer to the section below for the new technical overview.

* *hsh* adapts the Memento design pattern to achieve state rollback. Alterations had to be made to better suit the low-level C environment as the pattern itself was designed for object-oriented programming.

* There are designated command categories according to which arguments are parsed. The parsed command category is then matched to the corresponding memento type. There are 5 memento types: Create, Move, Delete, Internal and Hollow. Arguments are passed down to the dedicated memento creating function, which is the originator.

* Since mementos have to allocate space for numerous fields with the same lifetime, arena allocation is heavily utilized to optimize memento creation.

* Mementos are stored in an array, which functions as the undo stack. The Hollow type was introduced here to store a pointer to the memento as well as the type of the memento. The type is required later to unpack the Hollow memento.

* Hollow mementos are staged right before a command is executed, and are only pushed onto the undo stack after a successful execution.

* When `undo` is executed, the top memento is popped from the stack. It is then unpacked and the dedicated rollback function is executed based on the memento type inside.

* Restoring destructive commands is possible with a trash directory located in `~/.local/share/hsh/trash`, as it is required for preserving file content. Trash is cleaned on exit.

## Technical Overview - V2

* Rollback is now achieved by using overlayfs. Notably, this enables support for flags.

* Currently there are two overlays: one for root and one for home. The lower layers are the root and home directories, respectively. Merged, upper and work layers are in this shell's dedicated directory inside home.

* The new rollback engine does not parse commands. Instead, it reaps the changes a command made from the upper layer. Before each command runs, the mount namespace is unshared, all mount points starting from the root directory are recursively marked as private, overlay is mounted and the merged layer directory becomes the new root. As this is done inside a child process, everything remains untouched for the host and child ends up isolated in the merged layer.

* Since the root directory was the lower layer, from the child's perspective everything is normal and commands can run as usual. However, changes are staged in upperdir and are not applied to the lower layer directly.

* *hsh* recursively reaps the upper layer by classifying, recording, committing to the lower layer and then removing everything new in the upper layer.

* Since the mount namespace was unshared, the entire overlay setup is deleted automatically when the child exits.

* Each change, e.g. a newly created file, is stored in a memento. Because there can be multiple mementos, they are stored in an array in a hollow memento. When the upper layer is cleared, one hollow memento is generated and is pushed to the undo stack.

* The process of unpacking mementos for the actual rollback is similar to the V1 architecture.

## Current Limitations and Future Work - V1

> These are old limitations kept for educational purposes. Refer below for the updated section.

Currently, commands are mapped to their command type in a table. This narrows the applicability of rollback, as supported commands have to be manually entered into the table. This is the best way I could come up with to pre-process commands. This could be avoided by post-processing commands, which would require dynamically reacting to the changes made without knowing anything about the command. My latest work on this is still in progress and involves system call interception to achieve command post-processing.

Command behavior can be altered drastically by flags. The widely varying changes that flags bring is the reason they are omitted from rollback. Supporting flags would either require very advanced parsing, or it would be naturally supported by post-processing commands.

This project is centered around implementing state rollback in UNIX shells, thus it lacks support for complex parsing and quality of life features.

## Current Limitations and Future Work - V2

Although overlayfs formally supports virtual filesystems in the lower layer, this results in broken behavior. For example, if the user modifies a file in `/proc`, overlayfs copies up the file into upperdir. However, since procfs is a virtual filesystem and files there are interfaces to kernel memory, the user ends up with a useless text file in the upper layer. Similar behavior is observed in most virtual filesystems I tested. I also could not just leave them because overlayfs does not traverse past mount points in the lower layer, making essential system directories invisible to the shell. The best solution I found was to just bind these directories directly. However this disabled rollback in these directories since modifications were done directly to the host filesystem.

Currently, I am working on an update where the directories to be overlaid or bound will be customizable via a configuration.

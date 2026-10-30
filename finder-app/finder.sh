#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Expected 2 parameters, but $# were provided, silly goose!"
    echo "Usage: finder.sh DIRECTORY STRING"
    echo "(1) DIRECTORY: Directory in which to search through files for the string."
    echo "(2) STRING: String to search for."
    exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d $filesdir ]
then
    echo "$filesdir is not a directory, dummy"
    exit 1
fi

# grep -r: recursively search in all files and folders
# grep -l: Only return the file names that contain a match
# wc: Word count utility. -l options returns number of lines
X=$(grep -r -l "$searchstr" $filesdir | wc -l)
Y=$(grep -r "$searchstr" $filesdir | wc -l)

echo "The number of files are $X and the number of matching lines are $Y"
exit 0

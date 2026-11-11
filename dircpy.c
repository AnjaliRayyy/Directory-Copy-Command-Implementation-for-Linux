
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#define SIZE 1024

// Structure for sharing data between threads
struct Data
{
    char source[SIZE];
    char target[SIZE];

    int recursive;

    unsigned long long totalBytes;
    unsigned long long copiedBytes;

    int finished;
    int errors;

    pthread_mutex_t mutex;
};

// Function declarations
void calculateSize(char *path, struct Data *d);
void copyDirectory(char *src, char *dest, struct Data *d);
void copyFile(char *src, char *dest, struct Data *d);
void createDirectory(char *path, struct Data *d);

void *copyThread(void *arg);
void *progressThread(void *arg);


// Create a directory if it does not exist
void createDirectory(char *path, struct Data *d)
{
    struct stat st;

    if (mkdir(path, 0700) != 0)
    {
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            printf("\nCannot create directory: %s\n", path);
            d->errors++;
        }
    }
}


// Calculate total size of files to be copied
void calculateSize(char *path, struct Data *d)
{
    DIR *dir;
    struct dirent *entry;

    char src[SIZE];
    struct stat st;

    dir = opendir(path);

    if (dir == NULL)
    {
        printf("Cannot open directory: %s\n", path);
        d->errors++;
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        if (snprintf(src, SIZE, "%s/%s",
                     path, entry->d_name) >= SIZE)
        {
            d->errors++;
            continue;
        }

        if (lstat(src, &st) != 0)
        {
            d->errors++;
            continue;
        }

        if (S_ISREG(st.st_mode))
        {
            d->totalBytes += (unsigned long long)st.st_size;
        }
        else if (S_ISDIR(st.st_mode) && d->recursive)
        {
            calculateSize(src, d);
        }
    }

    closedir(dir);
}


// Copy contents of one file to another
void copyFile(char *src, char *dest, struct Data *d)
{
    FILE *sourceFile;
    FILE *targetFile;

    char buffer[4096];
    size_t bytesRead;
    size_t bytesWritten;

    struct stat st;

    // Open source file
    sourceFile = fopen(src, "rb");

    if (sourceFile == NULL)
    {
        printf("\nSkipping unreadable file: %s\n", src);
        d->errors++;
        return;
    }

    // Open destination file
    targetFile = fopen(dest, "wb");

    if (targetFile == NULL)
    {
        printf("\nCannot create destination file: %s\n", dest);
        fclose(sourceFile);
        d->errors++;
        return;
    }

    // Copy file contents in chunks
    while ((bytesRead = fread(buffer, 1, sizeof(buffer),
                              sourceFile)) > 0)
    {
        bytesWritten = fwrite(buffer, 1, bytesRead, targetFile);

        pthread_mutex_lock(&d->mutex);

        d->copiedBytes += bytesWritten;

        pthread_mutex_unlock(&d->mutex);

        if (bytesWritten != bytesRead)
        {
            printf("\nWrite error: %s\n", dest);
            d->errors++;
            break;
        }
    }

    if (ferror(sourceFile))
    {
        printf("\nRead error: %s\n", src);
        d->errors++;
    }

    fclose(sourceFile);
    fclose(targetFile);

    // Preserve permissions only if copying succeeded
    if (stat(src, &st) == 0)
    {
        if (chmod(dest, st.st_mode & 0777) != 0)
            d->errors++;
    }
}


// Recursively copy directory contents
void copyDirectory(char *src, char *dest, struct Data *d)
{
    DIR *dir;
    struct dirent *entry;

    char sourcePath[SIZE];
    char targetPath[SIZE];

    struct stat st;

    dir = opendir(src);

    if (dir == NULL)
    {
        printf("\nCannot open directory: %s\n", src);
        d->errors++;
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        // Ignore current and parent directory
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        if (snprintf(sourcePath, SIZE, "%s/%s",
                     src, entry->d_name) >= SIZE ||
            snprintf(targetPath, SIZE, "%s/%s",
                     dest, entry->d_name) >= SIZE)
        {
            printf("\nPath is too long.\n");
            d->errors++;
            continue;
        }

        if (lstat(sourcePath, &st) != 0)
        {
            printf("\nCannot access: %s\n", sourcePath);
            d->errors++;
            continue;
        }

        // If entry is a regular file
        if (S_ISREG(st.st_mode))
        {
            copyFile(sourcePath, targetPath, d);
        }

        // If entry is a directory
        else if (S_ISDIR(st.st_mode) && d->recursive)
        {
            createDirectory(targetPath, d);

            copyDirectory(sourcePath, targetPath, d);

            // Apply original directory permissions
            if (chmod(targetPath, st.st_mode & 0777) != 0)
                d->errors++;
        }

        // Other files, including symbolic links, are skipped
    }

    closedir(dir);
}


// Thread responsible for copying
void *copyThread(void *arg)
{
    struct Data *d = (struct Data *)arg;

    struct stat st;

    if (stat(d->source, &st) == 0)
    {
        copyDirectory(d->source, d->target, d);

        // Preserve root directory permissions
        if (chmod(d->target, st.st_mode & 0777) != 0)
            d->errors++;
    }

    pthread_mutex_lock(&d->mutex);
    d->finished = 1;
    pthread_mutex_unlock(&d->mutex);

    return NULL;
}


// Thread responsible for displaying progress
void *progressThread(void *arg)
{
    struct Data *d = (struct Data *)arg;

    unsigned long long copied;
    int finished;
    int percentage;

    while (1)
    {
        pthread_mutex_lock(&d->mutex);

        copied = d->copiedBytes;
        finished = d->finished;

        pthread_mutex_unlock(&d->mutex);

        if (d->totalBytes == 0)
            percentage = finished ? 100 : 0;
        else
            percentage = (int)((copied * 100) / d->totalBytes);

        if (percentage > 100)
            percentage = 100;

        printf("\rCopying Progress: %d%%", percentage);
        fflush(stdout);

        if (finished)
            break;

        sleep(1);
    }

    printf("\n");

    return NULL;
}


// Main function
int main(int argc, char *argv[])
{
    struct Data d = {0};

    pthread_t copy_tid;
    pthread_t progress_tid;

    struct stat st;

    char sourceReal[SIZE];
    char targetReal[SIZE];

    // Check command-line arguments
    if (argc < 3 || argc > 4)
    {
        printf("Usage: ./dircpy <sourcePath> <targetPath> [-r]\n");
        return 1;
    }

    // Check recursive option
    if (argc == 4)
    {
        if (strcmp(argv[3], "-r") != 0)
        {
            printf("Invalid option: %s\n", argv[3]);
            return 1;
        }

        d.recursive = 1;
    }

    // Store source and target paths
    if (strlen(argv[1]) >= SIZE || strlen(argv[2]) >= SIZE)
    {
        printf("Path is too long.\n");
        return 1;
    }

    strcpy(d.source, argv[1]);
    strcpy(d.target, argv[2]);

    // Check source directory
    if (stat(d.source, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        printf("Invalid source directory.\n");
        return 1;
    }

    // Create destination directory
    createDirectory(d.target, &d);

    if (d.errors > 0)
        return 1;

    // Prevent copying a directory into itself
    if (realpath(d.source, sourceReal) == NULL ||
        realpath(d.target, targetReal) == NULL)
    {
        printf("Unable to resolve directory paths.\n");
        return 1;
    }

    int len = strlen(sourceReal);

    if (strncmp(sourceReal, targetReal, len) == 0 &&
        (targetReal[len] == '/' || targetReal[len] == '\0'))
    {
        printf("Destination cannot be inside the source directory.\n");
        return 1;
    }

    // Calculate total bytes before copying
    calculateSize(d.source, &d);

    // Initialize mutex
    pthread_mutex_init(&d.mutex, NULL);

    printf("Starting directory copy...\n");

    // Create copying thread
    if (pthread_create(&copy_tid, NULL, copyThread, &d) != 0)
    {
        printf("Unable to create copy thread.\n");
        pthread_mutex_destroy(&d.mutex);
        return 1;
    }

    // Create progress thread
    if (pthread_create(&progress_tid, NULL, progressThread, &d) != 0)
    {
        printf("Unable to create progress thread.\n");

        pthread_join(copy_tid, NULL);
        pthread_mutex_destroy(&d.mutex);

        return 1;
    }

    // Wait for both threads
    pthread_join(copy_tid, NULL);
    pthread_join(progress_tid, NULL);

    pthread_mutex_destroy(&d.mutex);

    if (d.errors == 0)
        printf("Directory copied successfully!\n");
    else
        printf("Copy completed with %d error(s).\n", d.errors);

    printf("Total bytes: %llu\n", d.totalBytes);
    printf("Copied bytes: %llu\n", d.copiedBytes);

    return d.errors == 0 ? 0 : 1;
}

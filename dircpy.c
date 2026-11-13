#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#define SIZE 4096

// Shared information between threads
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

int isDirectory(char *path);
int resolveTargetPath(char *path, char *resolved);


// Check whether a path represents a directory
int isDirectory(char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}


// Resolve a destination path even if its final directories
// do not exist yet.
int resolveTargetPath(char *path, char *resolved)
{
    char temp[SIZE];
    char *slash;
    char *remaining;

    if (strlen(path) == 0 || strlen(path) >= SIZE)
        return -1;

    strcpy(temp, path);

    while (1)
    {
        // Try resolving the current path
        if (realpath(temp, resolved) != NULL)
        {
            remaining = path + strlen(temp);

            while (*remaining == '/')
                remaining++;

            if (*remaining == '\0')
                return 0;

            // Append the remaining non-existing path components
            if (strcmp(resolved, "/") == 0)
            {
                if (snprintf(resolved, SIZE, "/%s",
                             remaining) >= SIZE)
                    return -1;
            }
            else
            {
                char result[SIZE];

                if (snprintf(result, SIZE, "%s/%s",
                             resolved, remaining) >= SIZE)
                    return -1;

                strcpy(resolved, result);
            }

            return 0;
        }

        // Find the previous existing parent
        slash = strrchr(temp, '/');

        if (slash == NULL)
        {
            // Relative path: use current working directory
            if (getcwd(resolved, SIZE) == NULL)
                return -1;

            if (snprintf(temp, SIZE, "%s/%s",
                         resolved, path) >= SIZE)
                return -1;

            strcpy(resolved, temp);
            return 0;
        }

        if (slash == temp)
            temp[1] = '\0';
        else
            *slash = '\0';
    }
}


// Create a directory and all missing parent directories
void createDirectory(char *path, struct Data *d)
{
    char temp[SIZE];

    int i;
    int len = strlen(path);

    struct stat st;

    if (len == 0 || len >= SIZE)
    {
        printf("\nInvalid directory path.\n");
        d->errors++;
        return;
    }

    strcpy(temp, path);

    // Remove trailing slash
    if (len > 1 && temp[len - 1] == '/')
        temp[len - 1] = '\0';

    // Create missing parent directories
    for (i = 1; temp[i] != '\0'; i++)
    {
        if (temp[i] == '/')
        {
            temp[i] = '\0';

            if (mkdir(temp, 0700) != 0)
            {
                if (stat(temp, &st) != 0 || !S_ISDIR(st.st_mode))
                {
                    printf("\nCannot create directory: %s\n", temp);
                    perror("mkdir");
                    d->errors++;
                    return;
                }
            }

            temp[i] = '/';
        }
    }

    // Create final directory
    if (mkdir(temp, 0700) != 0)
    {
        if (stat(temp, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            printf("\nCannot create directory: %s\n", temp);
            perror("mkdir");
            d->errors++;
        }
    }
}


// Calculate total size of files
void calculateSize(char *path, struct Data *d)
{
    DIR *dir;
    struct dirent *entry;

    char src[SIZE];

    struct stat st;

    dir = opendir(path);

    if (dir == NULL)
    {
        printf("\nCannot examine directory: %s\n", path);
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
            printf("\nPath is too long.\n");
            d->errors++;
            continue;
        }

        if (lstat(src, &st) != 0)
        {
            printf("\nCannot access: %s\n", src);
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


// Copy one file
void copyFile(char *src, char *dest, struct Data *d)
{
    FILE *sourceFile;
    FILE *targetFile;

    char buffer[4096];

    size_t bytesRead;
    size_t bytesWritten;

    struct stat st;

    int success = 1;

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
            success = 0;
            break;
        }
    }

    // Detect read errors
    if (ferror(sourceFile))
    {
        printf("\nRead error: %s\n", src);
        d->errors++;
        success = 0;
    }

    // Close source file
    if (fclose(sourceFile) != 0)
    {
        printf("\nError closing source file: %s\n", src);
        d->errors++;
        success = 0;
    }

    // Close destination file
    // This also detects delayed buffered write errors
    if (fclose(targetFile) != 0)
    {
        printf("\nError closing destination file: %s\n", dest);
        d->errors++;
        success = 0;
    }

    // Preserve permissions only after successful copying
    if (success)
    {
        if (stat(src, &st) == 0)
        {
            if (chmod(dest, st.st_mode & 0777) != 0)
            {
                printf("\nCannot preserve permissions: %s\n", dest);
                d->errors++;
            }
        }
        else
        {
            printf("\nCannot retrieve source permissions: %s\n", src);
            d->errors++;
        }
    }
    else
    {
        // Remove incomplete destination file
        if (remove(dest) != 0)
        {
            printf("\nCannot remove incomplete file: %s\n", dest);
        }
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
        // Skip current and parent directory
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        // Construct complete paths
        if (snprintf(sourcePath, SIZE, "%s/%s",
                     src, entry->d_name) >= SIZE ||
            snprintf(targetPath, SIZE, "%s/%s",
                     dest, entry->d_name) >= SIZE)
        {
            printf("\nPath is too long.\n");
            d->errors++;
            continue;
        }

        // Get source entry information
        if (lstat(sourcePath, &st) != 0)
        {
            printf("\nCannot access: %s\n", sourcePath);
            d->errors++;
            continue;
        }

        // Handle regular files
        if (S_ISREG(st.st_mode))
        {
            copyFile(sourcePath, targetPath, d);
        }

        // Handle nested directories
        else if (S_ISDIR(st.st_mode) && d->recursive)
        {
            createDirectory(targetPath, d);

            // Check whether destination directory exists
            if (!isDirectory(targetPath))
                continue;

            copyDirectory(sourcePath, targetPath, d);

            // Restore original directory permissions
            if (chmod(targetPath, st.st_mode & 0777) != 0)
            {
                printf("\nCannot preserve directory permissions: %s\n",
                       targetPath);
                d->errors++;
            }
        }

        // Symbolic links and special files are skipped
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
        {
            printf("\nCannot preserve root directory permissions.\n");
            d->errors++;
        }
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
        {
            percentage = finished ? 100 : 0;
        }
        else
        {
            percentage = (int)(((double)copied /
                                d->totalBytes) * 100);
        }

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

    int len;
    int result;

    // Validate arguments
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

    // Validate path lengths
    if (strlen(argv[1]) >= SIZE || strlen(argv[2]) >= SIZE)
    {
        printf("Path is too long.\n");
        return 1;
    }

    strcpy(d.source, argv[1]);
    strcpy(d.target, argv[2]);

    // Validate source directory
    if (stat(d.source, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        printf("Invalid source directory.\n");
        return 1;
    }

    // Resolve source path
    if (realpath(d.source, sourceReal) == NULL)
    {
        printf("Unable to resolve source path.\n");
        return 1;
    }

    // Resolve destination before creating it
    if (resolveTargetPath(d.target, targetReal) != 0)
    {
        printf("Unable to resolve destination path.\n");
        return 1;
    }

    // Reject destination inside source or equal to source
    len = strlen(sourceReal);

    if (strncmp(sourceReal, targetReal, len) == 0 &&
        (targetReal[len] == '/' || targetReal[len] == '\0'))
    {
        printf("Destination cannot be inside the source directory.\n");
        return 1;
    }

    // Create destination and missing parent directories
    createDirectory(d.target, &d);

    if (d.errors > 0)
        return 1;

    // Calculate total file size
    calculateSize(d.source, &d);

    // Size-calculation errors are reported but do not prevent copying other accessible files.

    // Initialize mutex
    result = pthread_mutex_init(&d.mutex, NULL);

    if (result != 0)
    {
        printf("Unable to initialize mutex. Error: %d\n", result);
        return 1;
    }

    printf("Starting directory copy...\n");

    // Create copying thread
    result = pthread_create(&copy_tid, NULL, copyThread, &d);

    if (result != 0)
    {
        printf("Unable to create copy thread. Error: %d\n", result);
        pthread_mutex_destroy(&d.mutex);
        return 1;
    }

    // Create progress thread
    result = pthread_create(&progress_tid, NULL, progressThread, &d);

    if (result != 0)
    {
        printf("Unable to create progress thread. Error: %d\n", result);

        pthread_join(copy_tid, NULL);
        pthread_mutex_destroy(&d.mutex);

        return 1;
    }

    // Wait for copying thread
    result = pthread_join(copy_tid, NULL);

    if (result != 0)
        printf("Error joining copy thread: %d\n", result);

    // Wait for progress thread
    result = pthread_join(progress_tid, NULL);

    if (result != 0)
        printf("Error joining progress thread: %d\n", result);

    // Destroy mutex
    result = pthread_mutex_destroy(&d.mutex);

    if (result != 0)
        printf("Error destroying mutex: %d\n", result);

    // Display final result
    if (d.errors == 0)
        printf("Directory copied successfully!\n");
    else
        printf("Copy completed with %d error(s).\n", d.errors);

    printf("Total bytes: %llu\n", d.totalBytes);
    printf("Copied bytes: %llu\n", d.copiedBytes);

    return d.errors == 0 ? 0 : 1;
}

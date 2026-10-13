#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

int main(void) {
    char input[1024];

    while (1) {
        printf("qcsh> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\n");
            break;
        }
        
        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, "banao") == 0) {
        printf("Usage: banao filename\n");
        continue;
    }

    if (strncmp(input, "banao ", 6) == 0) {
        char *fileName = input + 6;

        while (*fileName == ' ')
            fileName++;

        if (*fileName == '\0') {
            printf("Please provide a filename\n");
            continue;
        }

        FILE *fptr = fopen(fileName, "w");

        if (fptr == NULL) {
            perror("Could not create file");
            continue;
        }

        fclose(fptr);
        printf("Created: %s\n", fileName);
        continue;
    }
    if (strcmp(input, "kholo") == 0) {
        printf("Usage: kholo foldername\n");
        continue;
    }

    if (strncmp(input, "kholo ", 6) == 0) {
        char *folderName = input + 6;

        while (*folderName == ' ')
            folderName++;

        if (*folderName == '\0') {
            printf("Please provide a folder name\n");
            continue;
        }

        if (chdir(folderName) == -1) {
            perror("Could not open folder");
            continue;
        }

        printf("Changed directory to: %s\n", folderName);
        continue;
    }
    if (strcmp(input, "banadalo") == 0) {
    printf("Usage: banadalo foldername\n");
    continue;
}

if (strncmp(input, "banadalo ", 9) == 0) {
    char *folderName = input + 9;

    while (*folderName == ' ')
        folderName++;

    if (*folderName == '\0') {
        printf("Please provide a folder name\n");
        continue;
    }

    if (mkdir(folderName, 0755) == -1) {
        perror("Could not create folder");
        continue;
    }

    printf("Created folder: %s\n", folderName);
    continue;
}
if (strcmp(input, "dikhao") == 0) {
    printf("Usage: dikhao filename\n");
    continue;
}

if (strncmp(input, "dikhao ", 7) == 0) {
    char *fileName = input + 7;

    while (*fileName == ' ')
        fileName++;

    if (*fileName == '\0') {
        printf("Please provide a filename\n");
        continue;
    }

    FILE *fptr = fopen(fileName, "r");

    if (fptr == NULL) {
        perror("Could not open file");
        continue;
    }

    int ch;

    while ((ch = fgetc(fptr)) != EOF)
        putchar(ch);

    if (ferror(fptr))
        fprintf(stderr, "\nError reading file\n");

    fclose(fptr);
    printf("\n");
    continue;
}

        if (strcmp(input, "exit") == 0)
            break;

        if (input[0] == '\0')
            continue;

        printf("You entered: %s\n", input);
    }

    return 0;
}
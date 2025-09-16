#include <unistd.h>
#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mutex>
#include <pthread.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <fstream>

using namespace std;

struct ParamLCG
{
    uint64_t X0;
    uint64_t A;
    uint64_t C;
    uint64_t M;
};

class LunarLandingModule
{
private:
    uint64_t count;
    uint64_t a, c, m;
    mutex mtx;

public:
    LunarLandingModule(uint64_t X0, uint64_t A, uint64_t C, uint64_t M)
    {
        count = X0;
        a = A;
        c = C;
        m = M;
    }

    uint64_t next()
    {
        lock_guard<mutex> lock(mtx);
        count = (a * count + c) % m;
        return static_cast<uint64_t>(count & 0xFF);
    }

    void generate(uint64_t *buffer, size_t length)
    {
        for (size_t i = 0; i < length; i++)
        {
            buffer[i] = next();
        }
    }
};

struct KeyGenArgs
{
    LunarLandingModule *LCG; 
    uint64_t *maskmass;     
    size_t file_size;       
    atomic<bool> *key_done;  
};

struct ContextWorker
{
    pthread_barrier_t *barrier;
    uint64_t *Mapped_data;
    uint64_t *Maskmass;
    uint64_t *Restext;
    size_t start;
    size_t end;
    atomic<bool> *shifrdone;
};

size_t get_CP()
{
    return thread::hardware_concurrency();
}

void *generate_key_thread(void *arg)
{
    KeyGenArgs *args = static_cast<KeyGenArgs *>(arg);
    args->LCG->generate(args->maskmass, args->file_size);
    args->key_done->store(true);

    return nullptr;
}

void *functionworker(void *arg)
{
    ContextWorker *context = static_cast<ContextWorker *>(arg);

    for (size_t i = context->start; i < context->end; i++)
    {
        context->Restext[i] = context->Mapped_data[i] ^ context->Maskmass[i];
    }
    pthread_barrier_wait(context->barrier);

    return nullptr;
}

bool write_result_to_file(const string &filename, const uint64_t *data, size_t size)
{
    ofstream out_file(filename, ios::binary);
    if (!out_file)
    {
        cerr << "Cannot open output file: " << filename << endl;
        return false;
    }

    out_file.write(reinterpret_cast<const char *>(data), size);

    if (!out_file)
    {
        cerr << "Failed to write output file " << endl;
        return false;
    }

    out_file.close();
    return true;
}

int main(int argc, char *argv[])
{

    ParamLCG prmLCG = {0, 0, 0, 0};
    string input_file, output_file;
    int opt = 0;
    while ((opt = getopt(argc, argv, "i:o:x:c:a:m:")) != -1)
    {
        switch (opt)
        {
        case 'i':
            input_file = optarg;
            break;
        case 'o':
            output_file = optarg;
            break;
        case 'x':
            prmLCG.X0 = stoull(optarg);
            break;
        case 'c':
            prmLCG.C = stoull(optarg);
            break;
        case 'a':
            prmLCG.A = stoull(optarg);
            break;
        case 'm':
            prmLCG.M = stoull(optarg);
            break;
        default:
            cerr << "Failed" << endl;
            return 1;
        }
    }

    if (prmLCG.A == 0 || prmLCG.M == 0)
    {
        cerr << "Empty a or m" << endl;
        return 1;
    }

    if (input_file.empty() || output_file.empty())
    {
        cerr << "Empty OUT or IN file\n"
             << endl;
        return 1;
    }

    int fd = open(input_file.c_str(), O_RDONLY);
    if (fd == -1)
    {
        cerr << "Cannot open input file: " << endl;
        return 1;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1)
    {
        cerr << "Cannot get file size" << endl;
        close(fd);
        return 1;
    }

    size_t file_size = sb.st_size;
    if (file_size == 0)
    {
        cerr << "No size" << endl;
        close(fd);
        return 1;
    }

    // 1гб
    const size_t MAX_FILE_SIZE = 1 * 1024 * 1024 * 1024;
    if (file_size > MAX_FILE_SIZE)
    {
        cerr << "File too large" << endl;
        close(fd);
        return 1;
    }

    uint64_t *mapped_data = static_cast<uint64_t *>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped_data == MAP_FAILED)
    {
        close(fd);
        cerr << "Failed mmap" << endl;
        return 1;
    }

    LunarLandingModule LCG(prmLCG.X0, prmLCG.A, prmLCG.C, prmLCG.M);

    uint64_t *maskmass = new uint64_t[file_size];
    atomic<bool> key_done(false);

    pthread_t key_thread;
    KeyGenArgs key_args = {&LCG, maskmass, file_size, &key_done};

    if (pthread_create(&key_thread, nullptr, generate_key_thread, &key_args) != 0)
    {
        cerr << "Failed key" << endl;
        munmap(mapped_data, file_size);
        delete[] maskmass;
        return 1;
    }

    pthread_join(key_thread, nullptr);

    size_t num_workers = get_CP();
    if (num_workers == 0)
    {
        num_workers = 1;
    }

    pthread_barrier_t barrierPthr;
    if (pthread_barrier_init(&barrierPthr, nullptr, num_workers + 1) != 0)
    {
        std::cerr << "Failed barrier" << std::endl;
        munmap(mapped_data, file_size);
        delete[] maskmass;
        return 1;
    }


    vector<ContextWorker> contexts(num_workers);
    vector<pthread_t> workers(num_workers);
    uint64_t *resulttext = new uint64_t[file_size];
    atomic<bool> shifr_done(false);

    size_t start_size = file_size / num_workers;
    size_t endsize = num_workers - 1;
    for (size_t i = 0; i < num_workers; ++i)
    {
        contexts[i].barrier = &barrierPthr;
        contexts[i].Mapped_data = mapped_data;
        contexts[i].Maskmass = maskmass;
        contexts[i].Restext = resulttext;
        contexts[i].start = i * start_size;
        contexts[i].end = (i != endsize) ? (i + 1) * start_size : file_size;
        contexts[i].shifrdone = &shifr_done;
    }

    for (size_t i = 0; i < num_workers; i++)
    {
        if (pthread_create(&workers[i], nullptr, functionworker, &contexts[i]) != 0)
        {
            cerr << "Failed to create worker thread " << endl;
            for (size_t j = 0; j < i; ++j)
            {
                pthread_cancel(workers[j]);
            }
            pthread_barrier_destroy(&barrierPthr);
            munmap(mapped_data, file_size);
            delete[] maskmass;
            delete[] resulttext;
            return 1;
        }
    }

    pthread_barrier_wait(&barrierPthr);

    for (size_t i = 0; i < num_workers; ++i)
    {
        pthread_join(workers[i], nullptr);
    }

    pthread_barrier_destroy(&barrierPthr);

    if (!write_result_to_file(output_file, resulttext, file_size))
    {
        cerr << "Failed to write result to file" << endl;
        munmap(mapped_data, file_size);
        delete[] maskmass;
        delete[] resulttext;
        return 1;
    }

    //cout << "Success output file" << endl;

    munmap(mapped_data, file_size);
    delete[] maskmass;
    delete[] resulttext;

    return 0;
}

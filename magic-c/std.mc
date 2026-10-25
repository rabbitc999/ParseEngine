module std;
import std::algorithms;

/* Magic-C 标准库前导模块。这里放可由 MC 自身表达的类型；算法由编译器签名接入 VM 胶水。 */

generic<T> enum Option {
    Value(T value), None;

    bool isValue() {
        bool result = false;
        match (self) {
            Option<T>::Value(value) => { result = true; }
            Option<T>::None => { result = false; }
        }
        return result;
    }

    bool isNone() { return !self.isValue(); }

    T unwrap() {
        T result;
        match (self) {
            Option<T>::Value(value) => { result = value; }
            Option<T>::None => { std::panic("called Option.unwrap() on None"); }
        }
        return result;
    }

    T unwrapOr(T fallback) {
        T result = fallback;
        match (self) {
            Option<T>::Value(value) => { result = value; }
            Option<T>::None => { }
        }
        return result;
    }

    T expect(string message) {
        T result;
        match (self) {
            Option<T>::Value(value) => { result = value; }
            Option<T>::None => { std::panic(message); }
        }
        return result;
    }

    Option<T> transform(fptr mapper(T value) -> T) {
        Option<T> result = Option<T>::None;
        match (self) {
            Option<T>::Value(value) => { result = Option<T>::Value(mapper(value)); }
            Option<T>::None => { }
        }
        return result;
    }

    Option<T> andThen(fptr mapper(T value) -> Option<T>) {
        Option<T> result = Option<T>::None;
        match (self) {
            Option<T>::Value(value) => { result = mapper(value); }
            Option<T>::None => { }
        }
        return result;
    }
};

generic<T, E> enum IResult {
    Ok(T value), Err(E error);

    bool isOk() {
        bool result = false;
        match (self) {
            IResult<T, E>::Ok(value) => { result = true; }
            IResult<T, E>::Err(error) => { result = false; }
        }
        return result;
    }

    bool isErr() { return !self.isOk(); }

    T unwrap() {
        T result;
        match (self) {
            IResult<T, E>::Ok(value) => { result = value; }
            IResult<T, E>::Err(error) => { std::panic("called IResult.unwrap() on Err"); }
        }
        return result;
    }

    T unwrapOr(T fallback) {
        T result = fallback;
        match (self) {
            IResult<T, E>::Ok(value) => { result = value; }
            IResult<T, E>::Err(error) => { }
        }
        return result;
    }

    T expect(string message) {
        T result;
        match (self) {
            IResult<T, E>::Ok(value) => { result = value; }
            IResult<T, E>::Err(error) => { std::panic(message); }
        }
        return result;
    }

    E error() {
        E result;
        match (self) {
            IResult<T, E>::Ok(value) => { std::panic("called IResult.error() on Ok"); }
            IResult<T, E>::Err(error) => { result = error; }
        }
        return result;
    }

    IResult<T, E> transform(fptr mapper(T value) -> T) {
        IResult<T, E> result;
        match (self) {
            IResult<T, E>::Ok(value) => { result = IResult<T, E>::Ok(mapper(value)); }
            IResult<T, E>::Err(error) => { result = IResult<T, E>::Err(error); }
        }
        return result;
    }

    IResult<T, E> andThen(fptr mapper(T value) -> IResult<T, E>) {
        IResult<T, E> result;
        match (self) {
            IResult<T, E>::Ok(value) => { result = mapper(value); }
            IResult<T, E>::Err(error) => { result = IResult<T, E>::Err(error); }
        }
        return result;
    }
};

enum ParseError { Empty, Invalid, Overflow };

int __parseInt64Error(string text);
int64 __parseInt64Value(string text);
int __parseUInt64Error(string text);
uint64 __parseUInt64Value(string text);
int __parseFloat64Error(string text);
f64 __parseFloat64Value(string text);
int __parseBoolError(string text);
bool __parseBoolValue(string text);

struct Parse {
    static IResult<int64, ParseError> parseInt64(string text) {
        int error = std::__parseInt64Error(text);
        if (error == 0) {
            return IResult<int64, ParseError>::Ok(std::__parseInt64Value(text));
        }
        if (error == 1) { return IResult<int64, ParseError>::Err(ParseError::Empty); }
        if (error == 2) { return IResult<int64, ParseError>::Err(ParseError::Overflow); }
        return IResult<int64, ParseError>::Err(ParseError::Invalid);
    }

    static IResult<uint64, ParseError> parseUInt64(string text) {
        int error = std::__parseUInt64Error(text);
        if (error == 0) {
            return IResult<uint64, ParseError>::Ok(std::__parseUInt64Value(text));
        }
        if (error == 1) { return IResult<uint64, ParseError>::Err(ParseError::Empty); }
        if (error == 2) { return IResult<uint64, ParseError>::Err(ParseError::Overflow); }
        return IResult<uint64, ParseError>::Err(ParseError::Invalid);
    }

    static IResult<f64, ParseError> parseFloat64(string text) {
        int error = std::__parseFloat64Error(text);
        if (error == 0) {
            return IResult<f64, ParseError>::Ok(std::__parseFloat64Value(text));
        }
        if (error == 1) { return IResult<f64, ParseError>::Err(ParseError::Empty); }
        if (error == 2) { return IResult<f64, ParseError>::Err(ParseError::Overflow); }
        return IResult<f64, ParseError>::Err(ParseError::Invalid);
    }

    static IResult<bool, ParseError> parseBool(string text) {
        int error = std::__parseBoolError(text);
        if (error == 0) {
            return IResult<bool, ParseError>::Ok(std::__parseBoolValue(text));
        }
        if (error == 1) { return IResult<bool, ParseError>::Err(ParseError::Empty); }
        return IResult<bool, ParseError>::Err(ParseError::Invalid);
    }
};

string __pathJoin(string left, string right);
string __pathFileName(string path);
string __pathExtension(string path);
string __pathNormalize(string path);

struct Path {
    static string join(string left, string right) { return std::__pathJoin(left, right); }
    static string fileName(string path) { return std::__pathFileName(path); }
    static string extension(string path) { return std::__pathExtension(path); }
    static string normalize(string path) { return std::__pathNormalize(path); }
};

uint64 __randomEntropy();

struct Random {
    uint64 state;

    static Random withSeed(uint64 seed) {
        Random result;
        result.state = seed;
        return result;
    }

    static Random fromEntropy() { return Random::withSeed(std::__randomEntropy()); }

    uint64 nextUInt64() {
        self.state = self.state * 6364136223846793005 + 1442695040888963407;
        return self.state;
    }

    int nextInt(int minimum, int maximum) {
        std::assert(maximum > minimum);
        uint64 width = (uint64)(maximum - minimum);
        return minimum + (int)(self.nextUInt64() % width);
    }

    f64 nextFloat64() {
        uint64 bits = self.nextUInt64() >> 11;
        return (f64)bits / 9007199254740992.0;
    }
};

generic<K, V> struct Pair {
    K first;
    V second;

    static Pair<K, V> of(K first, V second) {
        Pair<K, V> result;
        result.first = first;
        result.second = second;
        return result;
    }
};

enum IoError {
    OpenFailed(string message), ReadFailed(string message),
    WriteFailed(string message), SeekFailed(string message), Closed
};

enum SeekOrigin { Begin, Current, End };

interface IReader { IResult<string, IoError> read(); };
interface IWriter {
    IResult<int, IoError> write(string text);
    IResult<int, IoError> flush();
};
interface ISeek { IResult<int64, IoError> seek(int64 offset, SeekOrigin origin); };
interface IToString { string toString(); };
interface IFormattable { string toString(string style); };

string __fileTryRead(file handle);
int __fileTryWrite(file handle, string text);
int __fileTryFlush(file handle);
int64 __fileTrySeek(file handle, int64 offset, int origin);
bool __fileSucceeded(file handle);
string __fileLastError(file handle);
int __fileTryClose(file handle);

struct FileStream : IReader, IWriter, ISeek {
    file handle;

    static IResult<FileStream, IoError> open(string path, string mode) {
        IResult<file, string> opened = std::fileOpen(path, mode);
        IResult<FileStream, IoError> result;
        match (opened) {
            IResult<file, string>::Ok(handle) => {
                FileStream stream;
                stream.handle = handle;
                result = IResult<FileStream, IoError>::Ok(stream);
            }
            IResult<file, string>::Err(message) => {
                result = IResult<FileStream, IoError>::Err(IoError::OpenFailed(message));
            }
        }
        return result;
    }

    IResult<string, IoError> read() {
        string text = std::__fileTryRead(self.handle);
        if (std::__fileSucceeded(self.handle)) {
            return IResult<string, IoError>::Ok(text);
        }
        return IResult<string, IoError>::Err(
            IoError::ReadFailed(std::__fileLastError(self.handle)));
    }

    IResult<int, IoError> write(string text) {
        int count = std::__fileTryWrite(self.handle, text);
        if (std::__fileSucceeded(self.handle)) {
            return IResult<int, IoError>::Ok(count);
        }
        return IResult<int, IoError>::Err(
            IoError::WriteFailed(std::__fileLastError(self.handle)));
    }

    IResult<int, IoError> flush() {
        int code = std::__fileTryFlush(self.handle);
        if (std::__fileSucceeded(self.handle)) {
            return IResult<int, IoError>::Ok(code);
        }
        return IResult<int, IoError>::Err(
            IoError::WriteFailed(std::__fileLastError(self.handle)));
    }

    IResult<int64, IoError> seek(int64 offset, SeekOrigin origin) {
        int originCode = 0;
        match (origin) {
            SeekOrigin::Begin => { originCode = 0; }
            SeekOrigin::Current => { originCode = 1; }
            SeekOrigin::End => { originCode = 2; }
        }
        int64 position = std::__fileTrySeek(self.handle, offset, originCode);
        if (std::__fileSucceeded(self.handle)) {
            return IResult<int64, IoError>::Ok(position);
        }
        return IResult<int64, IoError>::Err(
            IoError::SeekFailed(std::__fileLastError(self.handle)));
    }

    int close() { return std::__fileTryClose(self.handle); }
};

generic<T> struct Deque : IIterator<T>, IIndex<T, int>, IDrop {
    std::List<T> values;

    static Deque<T> create() {
        Deque<T> result;
        std::List<T> values;
        result.values = values.copy();
        return result;
    }

    void pushBack(T value) { self.values.push(value); }
    void pushFront(T value) { self.values.insert(0, value); }
    Option<T> popBack() {
        if (self.values.len() == 0) { return Option<T>::None; }
        return Option<T>::Value(self.values.pop());
    }
    Option<T> popFront() {
        if (self.values.len() == 0) { return Option<T>::None; }
        T value = self.values.at(0);
        self.values.remove(0);
        return Option<T>::Value(value);
    }
    T index(int key) { return self.values.at(key); }
    Option<T> iterator(int index) {
        if (index < self.values.len()) { return Option<T>::Value(self.values.at(index)); }
        return Option<T>::None;
    }
    int len() { return self.values.len(); }
    bool isEmpty() { return self.values.len() == 0; }
    void clear() { self.values.clear(); }
    void drop() { delete self.values; }
};

generic<T> struct Queue : IIterator<T>, IDrop {
    std::List<T> values;

    static Queue<T> create() {
        Queue<T> result;
        std::List<T> values;
        result.values = values.copy();
        return result;
    }

    void push(T value) { self.values.push(value); }
    Option<T> pop() {
        if (self.values.len() == 0) { return Option<T>::None; }
        T value = self.values.at(0);
        self.values.remove(0);
        return Option<T>::Value(value);
    }
    Option<T> peek() {
        if (self.values.len() == 0) { return Option<T>::None; }
        return Option<T>::Value(self.values.at(0));
    }
    Option<T> iterator(int index) {
        if (index < self.values.len()) { return Option<T>::Value(self.values.at(index)); }
        return Option<T>::None;
    }
    int len() { return self.values.len(); }
    bool isEmpty() { return self.values.len() == 0; }
    void drop() { delete self.values; }
};

generic<T> struct RingBuffer : IIterator<T>, IDrop {
    std::List<T> values;
    int capacity;

    static RingBuffer<T> withCapacity(int capacity) {
        std::assert(capacity > 0);
        RingBuffer<T> result;
        std::List<T> values;
        result.values = values.copy();
        result.capacity = capacity;
        return result;
    }

    bool push(T value) {
        if (self.values.len() >= self.capacity) { return false; }
        self.values.push(value);
        return true;
    }
    Option<T> pop() {
        if (self.values.len() == 0) { return Option<T>::None; }
        T value = self.values.at(0);
        self.values.remove(0);
        return Option<T>::Value(value);
    }
    Option<T> iterator(int index) {
        if (index < self.values.len()) { return Option<T>::Value(self.values.at(index)); }
        return Option<T>::None;
    }
    int len() { return self.values.len(); }
    int available() { return self.capacity - self.values.len(); }
    void drop() { delete self.values; }
};

generic<T> struct FixedVector : IIterator<T>, IIndex<T, int>, IDrop {
    std::List<T> values;
    int capacity;

    static FixedVector<T> withCapacity(int capacity) {
        std::assert(capacity >= 0);
        FixedVector<T> result;
        std::List<T> values;
        result.values = values.copy();
        result.capacity = capacity;
        return result;
    }

    bool push(T value) {
        if (self.values.len() >= self.capacity) { return false; }
        self.values.push(value);
        return true;
    }
    Option<T> pop() {
        if (self.values.len() == 0) { return Option<T>::None; }
        return Option<T>::Value(self.values.pop());
    }
    T index(int key) { return self.values.at(key); }
    Option<T> iterator(int index) {
        if (index < self.values.len()) { return Option<T>::Value(self.values.at(index)); }
        return Option<T>::None;
    }
    int len() { return self.values.len(); }
    int capacityValue() { return self.capacity; }
    void drop() { delete self.values; }
};

generic<T> struct Span : IIterator<T>, IIndex<T, int> {
    T mut* data;
    int length;

    static Span<T> from(T mut* data, int length) {
        std::assert(length >= 0);
        Span<T> result;
        result.data = data;
        result.length = length;
        return result;
    }

    T index(int key) {
        std::assert(key >= 0 && key < self.length);
        return self.data[key];
    }
    Option<T> iterator(int index) {
        if (index >= 0 && index < self.length) { return Option<T>::Value(self.data[index]); }
        return Option<T>::None;
    }
    int len() { return self.length; }
};

struct MemoryStats {
    int64 liveBytes;
    int64 liveBlocks;
    int64 peakBytes;
};

int64 __memoryLiveBytes();
int64 __memoryLiveBlocks();
int64 __memoryPeakBytes();
uint64 __arenaCreate();
uint64 __arenaAllocate(uint64 arena, int bytes);
void __arenaReset(uint64 arena);
void __arenaDrop(uint64 arena);

struct Memory {
    static MemoryStats stats() {
        MemoryStats result;
        result.liveBytes = std::__memoryLiveBytes();
        result.liveBlocks = std::__memoryLiveBlocks();
        result.peakBytes = std::__memoryPeakBytes();
        return result;
    }
};

struct Arena : IDrop {
    uint64 handle;

    static Arena create() {
        Arena result;
        result.handle = std::__arenaCreate();
        return result;
    }

    Span<char> allocate(int bytes) {
        std::assert(bytes >= 0);
        uint64 address = std::__arenaAllocate(self.handle, bytes);
        return Span<char>::from((char mut*)address, bytes);
    }

    void reset() { std::__arenaReset(self.handle); }
    void drop() { std::__arenaDrop(self.handle); }
};

generic<T> struct Channel : IIterator<T>, IDrop {
    std::List<T> pending;

    static Channel<T> create() {
        Channel<T> result;
        std::List<T> pending;
        result.pending = pending.copy();
        return result;
    }

    void send(T value) { self.pending.push(value); }
    Option<T> receive() {
        if (self.pending.len() == 0) { return Option<T>::None; }
        T value = self.pending.at(0);
        self.pending.remove(0);
        return Option<T>::Value(value);
    }
    Option<T> iterator(int index) {
        if (index < self.pending.len()) { return Option<T>::Value(self.pending.at(index)); }
        return Option<T>::None;
    }
    int len() { return self.pending.len(); }
    void drop() { delete self.pending; }
};

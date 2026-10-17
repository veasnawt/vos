/* Test fixture (never shipped): thread-local storage, which ABI v0 forbids. */
__thread int counter;

int main(void)
{
    return counter;
}
